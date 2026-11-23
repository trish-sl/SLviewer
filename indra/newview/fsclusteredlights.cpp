/**
 * @file fsclusteredlights.cpp
 * @brief Clustered shading of the deferred local point lights, and their glow in fog
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsclusteredlights.h"
#include "fsblocktimer.h"
#include "fspointlightshadows.h"
#include "fswindfield.h"

#include "llagent.h"
#include "lldrawpoolalpha.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

extern U32 gFrameCount;
extern bool gSnapshotNoPost;

FSClusteredLights gFSClusteredLights;
LLGLSLShader gFSClusteredLightProgram;
LLGLSLShader gFSPointLightScatterProgram;

// Every view's binning and upload, probe faces included
static LLTrace::BlockTimerStatHandle FTM_FS_CLUSTERED_BIN("FS Clustered Lights Bin");
static LLTrace::BlockTimerStatHandle FTM_FS_CLUSTERED_UPLOAD("FS Clustered Lights Upload");

namespace
{
    // Depth slices start here: everything nearer is slice 0. Spaced evenly in log depth out to
    // the farthest light, so near slices are thin and far ones deep, as lights on screen are.
    constexpr F32 SLICE_NEAR = 1.f;
    // Relative slack on every depth bound, so a pixel the GPU puts on a slice boundary finds its
    // light on either side of it
    constexpr F32 DEPTH_SLACK = 1e-3f;
    // A cluster lists at most this many lights, the nearest first (a light's index is its rank in
    // the stock loop, which runs nearest first). Only reached when RenderLocalLightCount is raised
    // far past its default of 256.
    constexpr U32 MAX_PER_CLUSTER = 1024;
    // Lights past this many are left to the stock passes (RenderLocalLightCount caps the loop
    // long before)
    constexpr U32 MAX_LIGHTS = 65535;

    LLStaticHashedString s_lights("fs_cl_lights");
    LLStaticHashedString s_lists("fs_cl_lists");
    LLStaticHashedString s_dims("fs_cl_dims");
    LLStaticHashedString s_scale("fs_cl_scale");
    LLStaticHashedString s_debug("fs_cl_debug");
    LLStaticHashedString s_scatter_max("fs_scatter_max");
    LLStaticHashedString s_scatter_strength("fs_scatter_strength");
    LLStaticHashedString s_scatter_density("fs_scatter_density");
    LLStaticHashedString s_anisotropy("vol_anisotropy");
    LLStaticHashedString s_air_fog("fs_air_fog");
    LLStaticHashedString s_air_plane("fs_air_plane");
    LLStaticHashedString s_scatter_look("fs_scatter_look");
    LLStaticHashedString s_scatter_noise_origin("fs_scatter_noise_origin");
    LLStaticHashedString s_scatter_noise_basis("fs_scatter_noise_basis");
    LLStaticHashedString s_lls_count("fs_lls_count");
    LLStaticHashedString s_lls_index("fs_lls_index");

    // The point light glow: at most this many lights (drawScatter's clamp of
    // FSRenderPointLightScatterMaxLights), and the share of the last wanted light's score below
    // which the lights past it fade out
    constexpr U32 GLOW_MAX_LIGHTS = 64;
    constexpr F32 GLOW_FADE = 0.15f;
    // How many lights the glow takes at most for a max_lights setting: the setting plus room for
    // the lights that tie with the last wanted one or fade out just below it (chooseGlowLights)
    U32 glow_list_cap(U32 max_lights)
    {
        return llmin(GLOW_MAX_LIGHTS, max_lights + llmax(max_lights / 2, 4u));
    }
    // The natural glow's look: the share of even scattering blended into the Henyey-Greenstein
    // lobe, the size of the air's noise cells (metres) and the cells after which it repeats (as
    // fsPointLightScatterF.glsl's hash wraps)
    constexpr F32 GLOW_ISOTROPIC = 0.3f;
    constexpr F64 GLOW_NOISE_CELL = 2.0;
    constexpr F64 GLOW_NOISE_PERIOD = 256.0;
}

// static
// How much a light weighs on screen: brightness times the share of the view its sphere takes (its
// size over its distance from the camera, squared, capped where the camera is inside it).
// d2: squared distance from the camera to its centre.
F32 FSClusteredLights::screenWeight(F32 d2, F32 size, const LLColor3& linear_color)
{
    const F32 size2 = size * size;
    const F32 lum = 0.2126f * linear_color.mV[VX] + 0.7152f * linear_color.mV[VY] + 0.0722f * linear_color.mV[VZ];
    return lum * size2 / llmax(d2, size2, 0.01f);
}

// Moves the lights with a shadow map (FSRenderLocalLightShadows, add()'s shadow_slot) to the front
// of the list, keeping their order, and notes each one's slot in mMappedSlots. Only called before
// binning, so the lists and the glow follow the new order. Returns how many lead the list.
U32 FSClusteredLights::moveMappedLights()
{
    static_assert(FSPointLightShadows::MAX_LIGHTS <= sizeof(mMappedSlots) / sizeof(mMappedSlots[0]), "mMappedSlots too small");
    const U32 count = (U32)(mLights.size() / 2);
    if (mSlots.size() != count)
    {
        return 0;
    }
    U32 mapped = 0;
    for (U32 i = 0; i < count && mapped < FSPointLightShadows::MAX_LIGHTS; ++i)
    {
        if (mSlots[i] >= 0)
        {
            mMappedSlots[mapped++] = mSlots[i];
        }
    }
    if (mapped == 0)
    {
        return 0;
    }

    std::vector<LLVector4> sorted;
    sorted.reserve(mLights.size());
    U32 front = 0;
    for (U32 i = 0; i < count; ++i)
    {
        if (mSlots[i] >= 0 && front < mapped)
        {
            ++front;
            sorted.push_back(mLights[i * 2]);
            sorted.push_back(mLights[i * 2 + 1]);
        }
    }
    front = 0;
    for (U32 i = 0; i < count; ++i)
    {
        if (mSlots[i] >= 0 && front < mapped)
        {
            ++front;
            continue;
        }
        sorted.push_back(mLights[i * 2]);
        sorted.push_back(mLights[i * 2 + 1]);
    }
    mLights.swap(sorted);
    mSlots.clear(); // out of step with the list now
    return mapped;
}

// The point light glow's lights, chosen once for the whole screen: the max_lights whose glow weighs
// most on screen, scored as screenWeight scores (brightness times the share of the view the
// sphere takes). The glow used to take the first lights of each screen tile's list instead, so a
// light kept in one tile could be dropped in the next, where more lights reached: its halo was cut
// along the tile edges, in long straight lines where a big light's tiles ended. Now a light glows
// in every tile its sphere reaches or in none.
// So that a light changing places with the last one wanted does not pop, the weights follow the
// score of the max_lights-th light (ref): 1 at ref and above, falling to 0 at GLOW_FADE below it,
// and the lights in that band are taken too (a few past max_lights, glow_list_cap). Lights that
// tie with ref all glow in full: a set of identical lamps no longer loses its glow because one of
// them was left out (the fade used to end at the first light left out, so a tie gave weight 0).
// Only when the band holds more lights than the cap does the fade end at the first one it leaves
// out instead.
// Lights whose sphere lies wholly outside the view take no place: they glow on no pixel, and
// turning the camera would swap them in and out of the set. Not in the tiles of a high resolution
// snapshot, which must all choose the same lights. Fills mGlowWeights; returns how many lights
// were chosen.
U32 FSClusteredLights::chooseGlowLights(U32 max_lights)
{
    const U32 count = (U32)(mLights.size() / 2);
    mGlowWeights.assign(count, 0);
    max_lights = llclamp(max_lights, 1u, GLOW_MAX_LIGHTS);

    // The view's side planes (projection rows 3 +- 0 and 3 +- 1; reverse-Z only changes row 2),
    // for a sphere test in eye space
    glm::vec4 planes[4];
    F32 plane_scale[4] = { 0.f, 0.f, 0.f, 0.f };
    const bool frustum_test = LLViewerCamera::getInstance()->getZoomFactor() <= 1.f;
    if (frustum_test)
    {
        const glm::vec4 row0(mProjection[0][0], mProjection[1][0], mProjection[2][0], mProjection[3][0]);
        const glm::vec4 row1(mProjection[0][1], mProjection[1][1], mProjection[2][1], mProjection[3][1]);
        const glm::vec4 row3(mProjection[0][3], mProjection[1][3], mProjection[2][3], mProjection[3][3]);
        planes[0] = row3 + row0;
        planes[1] = row3 - row0;
        planes[2] = row3 + row1;
        planes[3] = row3 - row1;
        for (U32 p = 0; p < 4; ++p)
        {
            const F32 len = sqrtf(planes[p].x * planes[p].x + planes[p].y * planes[p].y + planes[p].z * planes[p].z);
            plane_scale[p] = len > 1e-6f ? 1.f / len : 0.f;
        }
    }

    std::vector<std::pair<F32, U32>> score;
    score.reserve(count);
    for (U32 i = 0; i < count; ++i)
    {
        const LLVector4& c = mLights[i * 2];
        const LLVector4& col = mLights[i * 2 + 1];
        const F32 size = c.mV[VW];
        if (size <= 0.f || -c.mV[VZ] + size <= 0.f)
        {
            continue; // no range, or wholly behind the camera
        }
        bool outside = false;
        for (U32 p = 0; p < 4 && !outside; ++p)
        {
            const F32 d = (planes[p].x * c.mV[VX] + planes[p].y * c.mV[VY] + planes[p].z * c.mV[VZ] + planes[p].w) * plane_scale[p];
            outside = plane_scale[p] > 0.f && d < -size;
        }
        if (outside)
        {
            continue; // wholly beside, above or below the view
        }
        const F32 lum = 0.2126f * col.mV[VX] + 0.7152f * col.mV[VY] + 0.0722f * col.mV[VZ];
        if (!(lum > 0.f))
        {
            continue;
        }
        const F32 size2 = size * size;
        const F32 d2 = c.mV[VX] * c.mV[VX] + c.mV[VY] * c.mV[VY] + c.mV[VZ] * c.mV[VZ];
        score.emplace_back(-lum * size2 / llmax(d2, size2, 0.01f), i); // negated: the strongest first
    }
    if (score.empty())
    {
        return 0;
    }

    // Strongest first; equal scores in list order, so the same lights win from frame to frame
    const U32 total = (U32)score.size();
    const U32 cap = llmin(total, glow_list_cap(max_lights));
    std::partial_sort(score.begin(), score.begin() + llmin(total, cap + 1), score.end());

    const F32 ref = -score[llmin(total, max_lights) - 1].first; // the last wanted light's score
    F32 fade_end = ref * (1.f - GLOW_FADE);                      // weight 0 here
    if (cap < total)
    {
        fade_end = llmax(fade_end, -score[cap].first);           // the band holds more than the cap
    }
    U32 chosen = 0;
    for (U32 k = 0; k < cap; ++k)
    {
        const F32 s = -score[k].first;
        F32 w = 1.f;
        if (s < ref)
        {
            w = ref > fade_end ? llclamp((s - fade_end) / (ref - fade_end), 0.f, 1.f) : 0.f;
        }
        const U16 weight = (U16)llclamp(ll_round(w * 65535.f), 0, 65535);
        if (weight == 0)
        {
            break; // sorted: the rest are weaker still
        }
        mGlowWeights[score[k].second] = weight;
        ++chosen;
    }
    return chosen;
}

bool FSClusteredLights::begin(const glm::mat4& modelview, const glm::mat4& projection, U32 width, U32 height, bool probe)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderClusteredLights", false);

    mLights.clear();
    mSlots.clear();
    mFarDepth = SLICE_NEAR;
    mActive = false;
    mShade = false;
    mBinned = false;
    mTileLists = false;
    mProbe = probe;
    if (probe)
    {   // a probe face's lights are no main view frame's: prepareScatter must not take them
        mFrame = U32_MAX;
    }

    const bool scatter = !probe && scatterWanted();
    if ((!enabled || !gFSClusteredLightProgram.isComplete()) && !scatter)
    {
        return false;
    }

    if (mMaxTexels < 0)
    {
        GLint texels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
        mMaxTexels = llmax(texels, 0);
        LL_INFOS("FSClusteredLights") << "Buffer textures hold up to " << mMaxTexels << " texels" << LL_ENDL;
    }
    mWidth = llmax(width, 1u);
    mHeight = llmax(height, 1u);

    if (!mLightBuffer)
    {
        glGenBuffers(1, &mLightBuffer);
        glGenBuffers(1, &mListBuffer);
        glGenTextures(1, &mLightTexture);
        glGenTextures(1, &mListTexture);
    }
    if (!mLightBuffer || !mListBuffer || !mLightTexture || !mListTexture || mMaxTexels < 65536)
    {
        return false;
    }

    // Lights handed over for shading must be drawn, so everything that could stop flush() from
    // drawing them is checked here, before the loop: room in the buffer texture for the grid's
    // header (clusters and tile lists) and a useful number of list entries, and two texture
    // units past the program's own samplers (three with the local light shadow maps)
    bool shade = enabled && gFSClusteredLightProgram.isComplete();
    const U64 header = (U64)((mWidth + TILE_PIXELS - 1) / TILE_PIXELS) * ((mHeight + TILE_PIXELS - 1) / TILE_PIXELS) * (DEPTH_SLICES + 1) * 2;
    if ((U64)mMaxTexels < header + 65536)
    {
        shade = false;
    }
    const S32 units = llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);
    const S32 extra_units = FSPointLightShadows::programSamples(gFSClusteredLightProgram) ? 3 : 2;
    if (shade && gFSClusteredLightProgram.mActiveTextureChannels + extra_units > units)
    {
        if (!mWarned)
        {
            mWarned = true;
            LL_WARNS("FSClusteredLights") << gFSClusteredLightProgram.mName << " uses " << gFSClusteredLightProgram.mActiveTextureChannels
                                          << " of " << units << " texture units, no room for the light lists" << LL_ENDL;
        }
        shade = false;
    }
    if (!shade && !scatter)
    {
        return false;
    }

    mModelview = modelview;
    mProjection = projection;
    mShade = shade;
    mActive = true;
    mFrame = probe ? U32_MAX : LLFrameTimer::getFrameCount();
    return true;
}

bool FSClusteredLights::add(const F32* center_agent, F32 size, const LLColor3& linear_color, F32 falloff, S32 shadow_slot)
{
    if (!mActive || mLights.size() >= (size_t)MAX_LIGHTS * 2)
    {
        return false;
    }
    // Eye space, as the stock passes transform their light centres (mul_mat4_vec3 of the modelview)
    const glm::vec3 c = mul_mat4_vec3(mModelview, glm::vec3(center_agent[0], center_agent[1], center_agent[2]));
    mLights.emplace_back(c.x, c.y, c.z, size);
    mLights.emplace_back(linear_color.mV[0], linear_color.mV[1], linear_color.mV[2], falloff);
    // the slot list is only kept once some light has a map (moveMappedLights): the lights before
    // that one had none
    if (shadow_slot >= 0 || !mSlots.empty())
    {
        mSlots.resize(mLights.size() / 2 - 1, -1);
        mSlots.push_back((S8)llclamp(shadow_slot, -1, 127));
    }
    mFarDepth = llmax(mFarDepth, -c.z + size);
    return mShade;
}

// Sorts the lights into the grid: mLists gets a header of (offset, count) per cluster (and with
// tile_lists, per screen tile after them), then the lists of light indices, each list nearest
// light first. The tile lists hold the glow's chosen lights only (chooseGlowLights), each entry
// with its weight in the high 16 bits.
void FSClusteredLights::binLights(bool tile_lists)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    FS_RECORD_BLOCK_TIME(FTM_FS_CLUSTERED_BIN);

    const U32 light_count = (U32)(mLights.size() / 2);
    if (tile_lists)
    {
        static LLCachedControl<S32> glow_max_lights(gSavedSettings, "FSRenderPointLightScatterMaxLights", 16);
        chooseGlowLights((U32)llmax((S32)glow_max_lights, 1));
    }
    mTilesX = (mWidth + TILE_PIXELS - 1) / TILE_PIXELS;
    mTilesY = (mHeight + TILE_PIXELS - 1) / TILE_PIXELS;
    mSlices = DEPTH_SLICES;
    mLogNear = logf(SLICE_NEAR);
    const F32 log_far = logf(llmax(mFarDepth, SLICE_NEAR * 2.f));
    mSliceScale = (F32)mSlices / (log_far - mLogNear);

    const F32 tiles_per_x = (F32)mWidth / (F32)TILE_PIXELS;  // tiles per unit of screen coordinate
    const F32 tiles_per_y = (F32)mHeight / (F32)TILE_PIXELS;
    const F32 pixel_x = 1.f / (F32)TILE_PIXELS;               // one pixel, in tiles
    const S32 last_x = (S32)mTilesX - 1;
    const S32 last_y = (S32)mTilesY - 1;

    // The slice of a depth, as fsClusteredLightF.glsl finds it
    auto slice_of = [&](F32 depth) -> U32
    {
        if (depth <= SLICE_NEAR)
        {
            return 0;
        }
        const F32 s = floorf((logf(depth) - mLogNear) * mSliceScale);
        return (U32)llclamp(s, 0.f, (F32)(mSlices - 1));
    };
    // The slice boundaries: slice k runs from mSliceBounds[k] to mSliceBounds[k + 1], the first from 0
    // and the last to F32_MAX
    mSliceBounds.resize(mSlices + 1);
    for (U32 k = 0; k <= mSlices; ++k)
    {
        mSliceBounds[k] = k == 0 ? 0.f : k >= mSlices ? F32_MAX : expf(mLogNear + (F32)k / mSliceScale);
    }
    auto slice_near = [&](U32 slice) -> F32
    {
        return mSliceBounds[slice];
    };
    auto slice_far = [&](U32 slice) -> F32
    {
        return mSliceBounds[slice + 1];
    };

    // The screen tiles a view space box covers, conservatively. False when it is off screen.
    auto box_tiles = [&](F32 x0, F32 x1, F32 y0, F32 y1, F32 d0, F32 d1, Span& span) -> bool
    {
        // mProjection * (x, y, -d, 1) at the eight corners from the terms of each axis, added as glm adds
        // them: (m[0] x + m[1] y) + (m[2] z + m[3]), so each corner comes out exactly as the full product
        const glm::vec4 term_x[2] = { mProjection[0] * x0, mProjection[0] * x1 };
        const glm::vec4 term_y[2] = { mProjection[1] * y0, mProjection[1] * y1 };
        const glm::vec4 term_z[2] = { mProjection[2] * -d0 + mProjection[3], mProjection[2] * -d1 + mProjection[3] };
        F32 min_x = F32_MAX, min_y = F32_MAX, max_x = -F32_MAX, max_y = -F32_MAX;
        bool whole = false;
        for (U32 corner = 0; corner < 8; ++corner)
        {
            const glm::vec4 p = (term_x[corner & 1] + term_y[(corner >> 1) & 1]) + term_z[(corner >> 2) & 1];
            if (p.w < 1e-4f)
            {
                whole = true; // reaches the camera plane: any pixel may look into it
                break;
            }
            const F32 sx = p.x / p.w * 0.5f + 0.5f;
            const F32 sy = p.y / p.w * 0.5f + 0.5f;
            min_x = llmin(min_x, sx);
            max_x = llmax(max_x, sx);
            min_y = llmin(min_y, sy);
            max_y = llmax(max_y, sy);
        }
        if (whole)
        {
            span.mX0 = 0;
            span.mY0 = 0;
            span.mX1 = (U32)last_x;
            span.mY1 = (U32)last_y;
            return true;
        }
        if (max_x < 0.f || max_y < 0.f || min_x > 1.f || min_y > 1.f)
        {
            return false;
        }
        span.mX0 = (U32)llclamp((S32)floorf(min_x * tiles_per_x - pixel_x), 0, last_x);
        span.mX1 = (U32)llclamp((S32)floorf(max_x * tiles_per_x + pixel_x), 0, last_x);
        span.mY0 = (U32)llclamp((S32)floorf(min_y * tiles_per_y - pixel_x), 0, last_y);
        span.mY1 = (U32)llclamp((S32)floorf(max_y * tiles_per_y + pixel_x), 0, last_y);
        return true;
    };

    // Per light and depth slice its sphere reaches: the box around the part of the sphere inside
    // the slice (its widest cross section there), and the tiles that box covers
    mSpans.clear();
    for (U32 i = 0; i < light_count; ++i)
    {
        const LLVector4& l = mLights[i * 2];
        const F32 s = l.mV[3];
        const F32 dc = -l.mV[2];
        const F32 dmin = dc - s;
        const F32 dmax = dc + s;
        if (dmax <= 0.f)
        {
            continue; // behind the camera
        }
        const U32 k0 = slice_of(llmax(dmin, 0.f) * (1.f - DEPTH_SLACK));
        const U32 k1 = slice_of(dmax * (1.f + DEPTH_SLACK));
        for (U32 k = k0; k <= k1; ++k)
        {
            const F32 a = llmax(slice_near(k) * (1.f - DEPTH_SLACK), dmin);
            const F32 b = llmin(slice_far(k) * (1.f + DEPTH_SLACK), dmax);
            if (a > b)
            {
                continue;
            }
            const F32 dz = llclamp(dc, a, b) - dc;
            const F32 rs = sqrtf(llmax(s * s - dz * dz, 0.f)) * (1.f + DEPTH_SLACK) + 1e-3f;
            Span span;
            span.mLight = i;
            span.mSlice = k;
            if (box_tiles(l.mV[0] - rs, l.mV[0] + rs, l.mV[1] - rs, l.mV[1] + rs, a, b, span))
            {
                mSpans.push_back(span);
            }
        }
        // The glow's tile list: every tile the whole sphere covers, any depth (chosen lights only)
        if (tile_lists && mGlowWeights[i] > 0)
        {
            const F32 rs = s * (1.f + DEPTH_SLACK) + 1e-3f;
            Span span;
            span.mLight = i;
            span.mSlice = mSlices;
            if (box_tiles(l.mV[0] - rs, l.mV[0] + rs, l.mV[1] - rs, l.mV[1] + rs, dmin, dmax, span))
            {
                mSpans.push_back(span);
            }
        }
    }

    // Count, cap, lay out the lists, fill them. Spans run light by light, so each list comes out
    // in light order, nearest first.
    const U32 entries = mTilesX * mTilesY * (mSlices + (tile_lists ? 1 : 0));
    mTileHeader = mTilesX * mTilesY * mSlices;
    mCounts.assign(entries, 0);
    for (const Span& span : mSpans)
    {
        for (U32 y = span.mY0; y <= span.mY1; ++y)
        {
            U32 idx = (span.mSlice * mTilesY + y) * mTilesX + span.mX0;
            for (U32 x = span.mX0; x <= span.mX1; ++x)
            {
                ++mCounts[idx++];
            }
        }
    }

    U32 cap = MAX_PER_CLUSTER;
    U64 total = 0;
    for (U32 count : mCounts)
    {
        total += llmin(count, cap);
    }
    const U64 room = (U64)mMaxTexels - (U64)entries * 2;
    if (total > room)
    {
        // Never at sane settings: keep the nearest lights of each cluster
        cap = llmax((U32)(room / entries), 1u);
        total = 0;
        for (U32 count : mCounts)
        {
            total += llmin(count, cap);
        }
        if (!mWarned)
        {
            mWarned = true;
            LL_WARNS("FSClusteredLights") << "Light lists overflow the buffer texture, each cluster keeps its nearest "
                                          << cap << " lights" << LL_ENDL;
        }
    }

    // The counts become the fill counts, zeroed as they are laid out
    mLists.resize((size_t)entries * 2 + (size_t)total);
    U32 offset = entries * 2;
    for (U32 i = 0; i < entries; ++i)
    {
        const U32 count = llmin(mCounts[i], cap);
        mLists[i * 2] = offset;
        mLists[i * 2 + 1] = count;
        offset += count;
        mCounts[i] = 0;
    }
    for (const Span& span : mSpans)
    {
        for (U32 y = span.mY0; y <= span.mY1; ++y)
        {
            U32 idx = (span.mSlice * mTilesY + y) * mTilesX + span.mX0;
            for (U32 x = span.mX0; x <= span.mX1; ++x, ++idx)
            {
                U32& fill = mCounts[idx];
                if (fill < mLists[idx * 2 + 1])
                {
                    mLists[mLists[idx * 2] + fill++] = span.mSlice == mSlices ? (span.mLight | ((U32)mGlowWeights[span.mLight] << 16)) : span.mLight;
                }
            }
        }
    }

    if (gFrameCount % 300 == 0)
    {
        LL_DEBUGS("FSClusteredLights") << light_count << " lights, " << mTilesX << "x" << mTilesY << "x" << mSlices
                                       << " clusters, " << total << " list entries" << LL_ENDL;
    }
}

void FSClusteredLights::upload()
{
    FS_RECORD_BLOCK_TIME(FTM_FS_CLUSTERED_UPLOAD);
    glBindBuffer(GL_TEXTURE_BUFFER, mLightBuffer);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(mLights.size() * sizeof(LLVector4)), mLights.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, mListBuffer);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(mLists.size() * sizeof(U32)), mLists.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    // Attached again after every upload: the stores were just replaced. Buffer textures have a
    // target of their own, so this leaves the unit's 2D binding that LLTexUnit tracks alone.
    glBindTexture(GL_TEXTURE_BUFFER, mLightTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, mLightBuffer);
    glBindTexture(GL_TEXTURE_BUFFER, mListTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, mListBuffer);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
}

void FSClusteredLights::bindBuffers(LLGLSLShader& shader)
{
    // Buffer samplers are not given units when the program is linked, so they take the first
    // two past the program's own (begin() checked there are two)
    const S32 base = shader.mActiveTextureChannels;
    mUnits[0] = base;
    mUnits[1] = base + 1;
    gGL.getTexUnit(mUnits[0])->activate();
    glBindTexture(GL_TEXTURE_BUFFER, mLightTexture);
    gGL.getTexUnit(mUnits[1])->activate();
    glBindTexture(GL_TEXTURE_BUFFER, mListTexture);
    gGL.getTexUnit(0)->activate();

    shader.uniform1i(s_lights, mUnits[0]);
    shader.uniform1i(s_lists, mUnits[1]);
    const GLint dims[4] = { (GLint)mTilesX, (GLint)mTilesY, (GLint)mSlices, (GLint)mTileHeader };
    shader.uniform4iv(s_dims, 1, dims);
    shader.uniform4f(s_scale, (F32)mWidth / (F32)TILE_PIXELS, (F32)mHeight / (F32)TILE_PIXELS, mSliceScale, mLogNear);
}

void FSClusteredLights::unbindBuffers()
{
    for (S32& unit : mUnits)
    {
        if (unit >= 0)
        {
            gGL.getTexUnit(unit)->activate();
            glBindTexture(GL_TEXTURE_BUFFER, 0);
            unit = -1;
        }
    }
    gGL.getTexUnit(0)->activate();
}

void FSClusteredLights::flush(LLVertexBuffer* screen_tri)
{
    if (!mActive)
    {
        return;
    }
    mActive = false;
    if (mLights.empty() || !mShade || !screen_tri)
    {
        return; // without shading, kept for the glow
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - clustered lights");
    LL_PROFILE_GPU_ZONE("clustered lights");

    // Lights with a shadow map (FSRenderLocalLightShadows) lead the list: the main view only,
    // never a probe face
    const bool shadow_maps = !mProbe && FSPointLightShadows::programSamples(gFSClusteredLightProgram);
    const U32 mapped = shadow_maps ? moveMappedLights() : 0;

    // The glow's tile lists are binned with the clusters, while the lights are at hand (not for
    // a probe face, which has no glow)
    mTileLists = !mProbe && scatterWanted();
    binLights(mTileLists);
    upload();
    mBinned = true;

    static LLCachedControl<bool> debug(gSavedSettings, "FSRenderClusteredLightsDebug", false);

    LLGLSLShader& shader = gFSClusteredLightProgram;
    LLGLDepthTest depth(GL_FALSE);
    gPipeline.bindDeferredShader(shader);
    bindBuffers(shader);
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    shader.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky && psky->canAutoAdjust()) ? 1 : 0);
    shader.uniform1i(s_debug, debug ? 1 : 0);
    // The shadow maps take the unit after the two buffer textures (begin() checked it is there)
    const bool maps_bound = gFSPointLightShadows.bind(shader, mUnits[1] + 1);
    if (maps_bound)
    {
        GLint index[FSPointLightShadows::MAX_LIGHTS] = {};
        for (U32 i = 0; i < mapped; ++i)
        {
            index[i] = mMappedSlots[i];
        }
        shader.uniform1i(s_lls_count, (GLint)mapped);
        shader.uniform1iv(s_lls_index, FSPointLightShadows::MAX_LIGHTS, index);
    }
    screen_tri->setBuffer();
    screen_tri->drawArrays(LLRender::TRIANGLES, 0, 3);
    if (maps_bound)
    {
        gFSPointLightShadows.unbind();
    }
    unbindBuffers();
    gPipeline.unbindDeferredShader(shader);
}

// static
bool FSClusteredLights::scatterWanted()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderPointLightScatter", false);
    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderPointLightScatterStrength", 1.f);
    return enabled && strength > 0.f && gFSPointLightScatterProgram.isComplete() && !gSnapshotNoPost &&
           !LLPipeline::sUnderWaterRender &&
           !(LLViewerCamera::getInstance()->cameraUnderWater() && LLWorld::getInstance()->getAllowRenderWater());
}

bool FSClusteredLights::prepareScatter(S32 max_lights)
{
    // Only this frame's lights: the light pass may not have collected any (local lights off,
    // a frame without the main view's light pass)
    if (mFrame != LLFrameTimer::getFrameCount() || mLights.empty() || max_lights <= 0)
    {
        return false;
    }
    if (mBinned)
    {
        return mTileLists; // the tile lists went up with the clusters (unless the glow was off then)
    }

    // Without clustered shading: one tile covering the screen, listing the chosen lights with
    // their weights, as the tile lists do
    const U32 count = chooseGlowLights((U32)max_lights);
    if (count == 0)
    {
        return false;
    }
    mTilesX = 1;
    mTilesY = 1;
    mSlices = 0;
    mTileHeader = 0;
    mSliceScale = 1.f;
    mLogNear = 0.f;
    mLists.resize(2 + (size_t)count);
    mLists[0] = 2;
    mLists[1] = count;
    U32 entry = 2;
    for (U32 i = 0; i < (U32)mGlowWeights.size() && entry < 2 + count; ++i)
    {
        if (mGlowWeights[i] > 0)
        {
            mLists[entry++] = i | ((U32)mGlowWeights[i] << 16);
        }
    }
    mLists[1] = entry - 2;
    upload();
    mBinned = true;
    mTileLists = true;
    return true;
}

bool FSClusteredLights::drawScatter(LLRenderTarget& depth_source, LLVertexBuffer* screen_tri, F32 strength_scale, F32 anisotropy, S32 max_lights)
{
    static LLCachedControl<F32> density(gSavedSettings, "FSRenderPointLightScatterDensity", 0.02f);
    static LLCachedControl<bool> classic(gSavedSettings, "FSRenderPointLightScatterClassic", false);
    static LLCachedControl<F32> glow_anisotropy(gSavedSettings, "FSRenderPointLightScatterAnisotropy", 0.5f);
    static LLCachedControl<F32> variation(gSavedSettings, "FSRenderPointLightScatterVariation", 0.35f);
    static LLCachedControl<F32> core_softness(gSavedSettings, "FSRenderPointLightScatterCoreSoftness", 1.f);

    LLGLSLShader& shader = gFSPointLightScatterProgram;
    const S32 units = llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);

    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("point light scatter");

    shader.bind();
    const bool drawn = shader.mActiveTextureChannels + 2 <= units;
    if (drawn)
    {
        shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_source, true);
        bindBuffers(shader);
        // the lists hold the chosen lights only, which may run a few past the setting (ties)
        shader.uniform1i(s_scatter_max, (S32)glow_list_cap((U32)llclamp(max_lights, 1, (S32)GLOW_MAX_LIGHTS)));
        shader.uniform1f(s_scatter_strength, strength_scale);
        shader.uniform1f(s_scatter_density, llclamp((F32)density, 0.f, 1.f));
        // The classic look keeps the projector cones' anisotropy, as it always had; the natural
        // look has its own
        shader.uniform1f(s_anisotropy, llclamp(classic ? anisotropy : (F32)glow_anisotropy, 0.f, 0.9f));
        // w: the natural look's soft centre, from a lamp-sized core (0) to an even glow with no
        // bright dot at the light (1)
        shader.uniform4f(s_scatter_look, classic ? 1.f : 0.f, llclamp((F32)variation, 0.f, 1.f), GLOW_ISOTROPIC,
                         llclamp((F32)core_softness, 0.f, 1.f));
        if (!classic && variation > 0.f)
        {
            // The air's noise is fixed in the world (it moves with the wind's drift when the wind
            // effects are on): the camera in noise cells, wrapped where the noise repeats, and the
            // world's axes in view space (the modelview's columns, the lights' own view space)
            const FSWindField::State& wind = FSWindField::get();
            LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
            if (wind.mActive)
            {
                camera -= wind.mDrift;
            }
            F32 origin[3];
            for (S32 i = 0; i < 3; ++i)
            {
                F64 c = fmod(camera.mdV[i] / GLOW_NOISE_CELL, GLOW_NOISE_PERIOD);
                if (c < 0.0)
                {
                    c += GLOW_NOISE_PERIOD;
                }
                origin[i] = (F32)c;
            }
            const F32 cells = (F32)(1.0 / GLOW_NOISE_CELL);
            F32 basis[9];
            for (S32 axis = 0; axis < 3; ++axis)
            {
                for (S32 k = 0; k < 3; ++k)
                {
                    basis[axis * 3 + k] = mModelview[axis][k] * cells;
                }
            }
            shader.uniform3fv(s_scatter_noise_origin, 1, origin);
            shader.uniform3fv(s_scatter_noise_basis, 3, basis);
        }
        setAirUniforms(shader, true);
        screen_tri->setBuffer();
        screen_tri->drawArrays(LLRender::TRIANGLES, 0, 3);
        unbindBuffers();
    }
    shader.unbind();
    return drawn;
}

// static
void FSClusteredLights::setAirUniforms(LLGLSLShader& shader, bool use_fog)
{
    // The height fog's values as fs_upload_height_fog_uniforms (llviewershadermgr.cpp) sends them
    static LLCachedControl<bool> fog_on(gSavedSettings, "FSRenderHeightFog", false);
    static LLCachedControl<F32> fog_density(gSavedSettings, "FSRenderHeightFogDensity", 0.003f);
    static LLCachedControl<F32> fog_falloff(gSavedSettings, "FSRenderHeightFogFalloff", 0.02f);
    static LLCachedControl<F32> fog_base(gSavedSettings, "FSRenderHeightFogBaseHeight", 0.f);
    static LLCachedControl<bool> fog_base_absolute(gSavedSettings, "FSRenderHeightFogBaseAbsolute", false);
    static LLCachedControl<bool> fog_sky_haze(gSavedSettings, "FSRenderHeightFogSkyHaze", true);

    // Only while the fog itself is drawn (its define is set once the shaders were rebuilt)
    if (!use_fog || !fog_on || !LLGLSLShader::sGlobalDefines.count("FS_HEIGHT_FOG"))
    {
        shader.uniform4f(s_air_fog, 0.f, 0.f, 0.f, 0.f);
        shader.uniform4f(s_air_plane, 0.f, 0.f, 1.f, 0.f);
        return;
    }

    LLEnvironment& environment = LLEnvironment::instance();
    F32 sigma = llclamp((F32)fog_density, 0.f, 0.1f);
    LLSettingsSky::ptr_t psky = environment.getCurrentSky();
    if (fog_sky_haze && psky)
    {
        sigma *= llclamp(sqrtf(llmax(psky->getHazeDensity(), 0.f) / 0.7f), 0.5f, 2.f);
    }
    sigma *= FSWindField::fogDensityScale(); // the wind's gusts (FSRenderWindEffects), 1 without
    F32 base = llclamp((F32)fog_base, -4096.f, 8192.f);
    if (fog_base_absolute)
    {
        base -= environment.getWaterHeight();
    }

    // Height above the fog's base of a view space point p: dot(p, plane.xyz) + plane.w, from the
    // water plane (view space, normalised) the haze pass gives the fog
    const LLVector4& water = LLDrawPoolAlpha::sWaterPlane;
    shader.uniform4f(s_air_fog, sigma, llclamp((F32)fog_falloff, 0.f, 1.f), 0.f, 1.f);
    shader.uniform4f(s_air_plane, water.mV[VX], water.mV[VY], water.mV[VZ], water.mV[VW] - base);
}

void FSClusteredLights::release()
{
    if (mLightTexture)
    {
        glDeleteTextures(1, &mLightTexture);
        mLightTexture = 0;
    }
    if (mListTexture)
    {
        glDeleteTextures(1, &mListTexture);
        mListTexture = 0;
    }
    if (mLightBuffer)
    {
        glDeleteBuffers(1, &mLightBuffer);
        mLightBuffer = 0;
    }
    if (mListBuffer)
    {
        glDeleteBuffers(1, &mListBuffer);
        mListBuffer = 0;
    }
    mLights.clear();
    mLights.shrink_to_fit();
    mSlots.clear();
    mSlots.shrink_to_fit();
    mSpans.clear();
    mSpans.shrink_to_fit();
    mCounts.clear();
    mCounts.shrink_to_fit();
    mSliceBounds.clear();
    mSliceBounds.shrink_to_fit();
    mLists.clear();
    mLists.shrink_to_fit();
    mGlowWeights.clear();
    mGlowWeights.shrink_to_fit();
    mActive = false;
    mShade = false;
    mBinned = false;
}
