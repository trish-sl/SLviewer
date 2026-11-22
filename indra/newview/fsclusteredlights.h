/**
 * @file fsclusteredlights.h
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

#ifndef FS_CLUSTEREDLIGHTS_H
#define FS_CLUSTEREDLIGHTS_H

#include "llglslshader.h"
#include "v3color.h"
#include "v4math.h"

#include "glm/mat4x4.hpp"

#include <vector>

class LLRenderTarget;
class LLVertexBuffer;

// The local point lights of the main view, shaded in one full screen pass (FSRenderClusteredLights)
// instead of a light volume or a share of a full screen pass each.
//
// LLPipeline::renderDeferredLighting still decides which lights are drawn: RenderLocalLightCount,
// attached lights, the size and colour cut-offs and the frustum test all run in the stock loop,
// which hands each point light that passes them to add() instead of drawing it. flush() then sorts
// them into clusters, the cells of a grid of 64 pixel screen tiles by 16 depth slices (spaced
// evenly in log depth), each cluster listing the lights whose sphere can reach into it: per depth
// slice, the part of the sphere inside the slice is boxed and the screen tiles that box covers
// get the light, a conservative test. Light data and the lists go up as two buffer textures, and
// fsClusteredLightF.glsl shades every pixel with the lights of its own cluster only, with the
// stock point light maths. Projectors, spotlights and reflection probe captures keep the stock
// passes. The binning runs on the CPU, so it works on OpenGL 3.3 and macOS alike.
//
// The same lights glow in the air (FSRenderPointLightScatter): fsPointLightScatterF.glsl adds
// their in-scattering into the projector volumetric map (LLPipeline::renderSpotVolumetrics), so
// it shares that map's resolution, temporal resolve and composite. The glowing lights are chosen
// once for the whole screen (chooseGlowLights): the FSRenderPointLightScatterMaxLights that weigh
// most on screen, the last few fading as they near the cut, so a light glows everywhere or
// nowhere. With clustered shading on, each pixel takes the chosen lights of its screen tile
// (binned with the clusters); without it, all of them. The lights are collected in the same stock
// loop, so the glow follows every setting that decides which lights are drawn.
class FSClusteredLights
{
public:
    static constexpr U32 TILE_PIXELS = 64;
    static constexpr U32 DEPTH_SLICES = 16;

    // Start of the main view's local light pass, before the stock loop. Clears the light list.
    // True when the loop should hand its point lights to add(): clustered shading is on and its
    // program, buffer textures and texture units are usable (everything that could stop flush()
    // from drawing the lights is checked here), or the glow wants them.
    // probe: a reflection probe face (FSPerfProbeFastLights) with its own camera and target: shading
    // only, no glow, and the lists the main view's glow reads later in the frame stay out of it.
    bool begin(const glm::mat4& modelview, const glm::mat4& projection, U32 width, U32 height, bool probe = false);

    // One point light that passed the stock loop's tests: agent space centre, volume half size
    // (radius * 1.5, as the stock path sends), linear colour already scaled, and falloff. True
    // when clustered shading draws it; false when the stock passes must still draw it (shading
    // off, the light only kept for the glow).
    bool add(const F32* center_agent, F32 size, const LLColor3& linear_color, F32 falloff);

    // After the stock loop: bin, upload and draw the lights into the bound lighting target
    // (additive blend, as the stock light passes). screen_tri is the full screen triangle.
    void flush(LLVertexBuffer* screen_tri);

    // Point light glow (renderSpotVolumetrics). True when it is on and can run this frame: the
    // setting and strength, its program, not under water nor in a snapshot without post effects.
    static bool scatterWanted();
    // Gets the lists ready for the glow: this frame's tile lists after clustered shading, else
    // one list of the chosen lights (chooseGlowLights). False when there is nothing to draw.
    bool prepareScatter(S32 max_lights);
    // Draws the glow into the bound target (the projector volumetric map, cleared, additive
    // blend), writing the distance each pixel sees into alpha as that map wants. strength_scale
    // is the glow's strength over the strength the map's composite applies; anisotropy the
    // projector cones', which only the classic look (FSRenderPointLightScatterClassic) takes.
    // False when nothing was drawn (no texture units left for the lists).
    bool drawScatter(LLRenderTarget& depth_source, LLVertexBuffer* screen_tri, F32 strength_scale, F32 anisotropy, S32 max_lights);

    // Air density uniforms of the volumetric shaders (fs_air_fog, fs_air_plane). With use_fog
    // and the height fog on, the air is the height fog's: its density at the fog's base and its
    // falloff with height, measured from the water plane as fsHeightFogF.glsl does. Otherwise
    // fs_air_fog.w is 0 and the shader keeps its constant density.
    static void setAirUniforms(LLGLSLShader& shader, bool use_fog);

    // Release the GL objects (LLPipeline::cleanup).
    void release();

private:
    U32 sortShadowedLights(U32 wanted);
    // The glow's lights (mGlowWeights), see the .cpp. Returns how many were chosen.
    U32 chooseGlowLights(U32 max_lights);
    void binLights(bool tile_lists);
    void upload();
    // Binds the two buffer textures to texture units past the shader's own samplers and sets
    // the grid uniforms.
    void bindBuffers(LLGLSLShader& shader);
    void unbindBuffers();

    struct Span
    {
        U32 mLight;
        U32 mSlice;     // DEPTH_SLICES for a screen tile list
        U32 mX0, mX1, mY0, mY1; // tiles, inclusive
    };

    glm::mat4 mModelview = glm::mat4(1.f);
    glm::mat4 mProjection = glm::mat4(1.f);
    U32 mWidth = 1;
    U32 mHeight = 1;
    bool mActive = false;               // collecting lights between begin() and flush()
    bool mShade = false;                // and shading them
    bool mBinned = false;               // the buffers hold this frame's lists
    bool mTileLists = false;            // and those include the glow's tile lists
    bool mProbe = false;                // collecting for a reflection probe face
    U32 mFrame = 0;                     // frame of the last begin() that collected for the main view

    std::vector<LLVector4> mLights;     // per light: eye space centre + size, colour + falloff
    F32 mFarDepth = 1.f;                // farthest depth any light reaches

    // Grid of the last binning
    U32 mTilesX = 1;
    U32 mTilesY = 1;
    U32 mSlices = 1;
    U32 mTileHeader = 0;                // first tile list entry of the header
    F32 mSliceScale = 1.f;              // slices per unit of log depth
    F32 mLogNear = 0.f;

    std::vector<Span> mSpans;
    std::vector<U32> mCounts;           // per cluster: its light count, then its fill while binning
    std::vector<F32> mSliceBounds;      // depth slice boundaries of the last binning
    std::vector<U32> mLists;            // header (offset, count per cluster, then per tile list),
                                        // then the light indices (in the tile lists, the glow's
                                        // weight in the high 16 bits)
    std::vector<U16> mGlowWeights;      // per light: its glow's weight, 0 (no glow) to 65535

    U32 mLightBuffer = 0;
    U32 mLightTexture = 0;
    U32 mListBuffer = 0;
    U32 mListTexture = 0;
    S32 mMaxTexels = -1;                // GL_MAX_TEXTURE_BUFFER_SIZE, queried once
    S32 mUnits[2] = { -1, -1 };         // texture units of the bound buffers
    bool mWarned = false;
};

extern FSClusteredLights gFSClusteredLights;

extern LLGLSLShader gFSClusteredLightProgram; // fsClusteredLightF.glsl, built in loadShadersDeferred
extern LLGLSLShader gFSPointLightScatterProgram; // fsPointLightScatterF.glsl, a post effect program

#endif // FS_CLUSTEREDLIGHTS_H
