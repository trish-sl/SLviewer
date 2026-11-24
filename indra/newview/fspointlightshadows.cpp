/**
 * @file fspointlightshadows.cpp
 * @brief Shadow maps of the local point lights (FSRenderLocalLightShadows)
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

#include "fspointlightshadows.h"
#include "fsblocktimer.h"
#include "fsclusteredlights.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcamera.h"
#include "lldrawable.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llvertexbuffer.h"
#include "llvovolume.h"
#include "pipeline.h"

#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <cmath>

extern bool gCubeSnapshot;

FSPointLightShadows gFSPointLightShadows;

static LLTrace::BlockTimerStatHandle FTM_FS_LOCAL_LIGHT_SHADOWS("FS Local Light Shadows");

// Texels around each face drawn past its 90 degrees: the 3x3 filter reaches 1.5 texels out
static constexpr U32 FS_LLS_GUARD_TEXELS = 3;
// Shadow strength a slot gains or loses per second as its light comes or goes
static constexpr F32 FS_LLS_FADE_RATE = 4.f;
// A light holding a slot keeps it until another weighs this much more on screen
static constexpr F32 FS_LLS_HOLD_BONUS = 1.5f;
// A kept face is drawn again after this many seconds, for anything the change notes miss
static constexpr F32 FS_LLS_MAX_AGE = 4.f;
// Shadows fade out over this share of FSRenderLocalLightShadowDistance
static constexpr F32 FS_LLS_DISTANCE_FADE = 0.2f;
// A face whose static casters could not be kept is drawn whole for this long before trying again
static constexpr F32 FS_LLS_SPLIT_RETRY = 2.f;

// The faces' axes in agent space: the direction each looks along, its right and its up. The view
// of face f has these as its rows (right, up, -forward), so a direction d lands at
// (dot(right, d), dot(up, d)) / dot(forward, d) on it, which fsPointShadowMapF.glsl spells out
// per face. Proper rotations, so front faces keep their winding.
static const F32 FS_LLS_FACE_AXES[FSPointLightShadows::FACES][3][3] =
{
    { {  1.f,  0.f,  0.f }, {  0.f, -1.f,  0.f }, { 0.f, 0.f, 1.f } }, // +X
    { { -1.f,  0.f,  0.f }, {  0.f,  1.f,  0.f }, { 0.f, 0.f, 1.f } }, // -X
    { {  0.f,  1.f,  0.f }, {  1.f,  0.f,  0.f }, { 0.f, 0.f, 1.f } }, // +Y
    { {  0.f, -1.f,  0.f }, { -1.f,  0.f,  0.f }, { 0.f, 0.f, 1.f } }, // -Y
    { {  0.f,  0.f,  1.f }, { -1.f,  0.f,  0.f }, { 0.f, 1.f, 0.f } }, // +Z
    { {  0.f,  0.f, -1.f }, {  1.f,  0.f,  0.f }, { 0.f, 1.f, 0.f } }, // -Z
};

// static
bool FSPointLightShadows::wantedByShaders()
{
    static LLCachedControl<S32> count(gSavedSettings, "FSRenderLocalLightShadows", 0);
    return count > 0 && gSavedSettings.getS32("RenderShadowDetail") > 0;
}

// static
bool FSPointLightShadows::programSamples(const LLGLSLShader& shader)
{
    return shader.mProgramObject && shader.mDefines.count("FS_POINT_SHADOW_MAP") > 0;
}

U32 FSPointLightShadows::lightCount() const
{
    static LLCachedControl<S32> count(gSavedSettings, "FSRenderLocalLightShadows", 0);
    if (count <= 0 || LLPipeline::RenderShadowDetail <= 0 || !LLPipeline::sRenderDeferred)
    {
        return 0;
    }
    if (!programSamples(gDeferredLightProgram) && !programSamples(gFSClusteredLightProgram))
    {
        return 0; // built without the lookup (the setting was off then, or the build fell back)
    }
    return (U32)llclamp((S32)count, 1, (S32)MAX_LIGHTS);
}

F32 FSPointLightShadows::projectionScale() const
{
    // the face's own 90 degrees end FS_LLS_GUARD_TEXELS short of its edges
    return mRes > FS_LLS_GUARD_TEXELS * 4 ? (F32)(mRes - 2 * FS_LLS_GUARD_TEXELS) / (F32)mRes : 1.f;
}

glm::mat4 FSPointLightShadows::faceView(U32 face, const LLVector3& eye) const
{
    const F32* f = FS_LLS_FACE_AXES[face][0];
    const F32* r = FS_LLS_FACE_AXES[face][1];
    const F32* u = FS_LLS_FACE_AXES[face][2];
    glm::mat4 view(1.f);
    for (U32 i = 0; i < 3; ++i)
    {
        view[i][0] = r[i];
        view[i][1] = u[i];
        view[i][2] = -f[i];
    }
    view[3][0] = -(r[0] * eye.mV[0] + r[1] * eye.mV[1] + r[2] * eye.mV[2]);
    view[3][1] = -(u[0] * eye.mV[0] + u[1] * eye.mV[1] + u[2] * eye.mV[2]);
    view[3][2] = f[0] * eye.mV[0] + f[1] * eye.mV[1] + f[2] * eye.mV[2];
    return view;
}

// A symmetric perspective in the standard OpenGL form (depth -1 to 1), square, slightly wider than
// 90 degrees by the guard band. fsPointShadowMapF.glsl takes the window depth at distance m along
// the face's axis as 0.5 * (A - B / m) + 0.5 with A and B from beginLightPass.
glm::mat4 FSPointLightShadows::faceProjection(F32 znear, F32 zfar) const
{
    const F32 k = projectionScale();
    glm::mat4 proj(0.f);
    proj[0][0] = k;
    proj[1][1] = k;
    proj[2][2] = -(zfar + znear) / (zfar - znear);
    proj[2][3] = -1.f;
    proj[3][2] = -2.f * zfar * znear / (zfar - znear);
    return proj;
}

bool FSPointLightShadows::holdsAnything() const
{
    if (mAtlas.isComplete())
    {
        return true;
    }
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        if (mSlots[i].mLight.notNull() || mWanted[i].notNull())
        {
            return true;
        }
    }
    return false;
}

void FSPointLightShadows::clearSlot(U32 slot)
{
    if (slot >= MAX_LIGHTS)
    {
        return;
    }
    Slot& s = mSlots[slot];
    s.mLight = nullptr;
    s.mReady = false;
    s.mFade = 0.f;
    s.mStrength = 0.f;
    for (LLPipeline::SpotShadowState& state : mFaces[slot])
    {
        state.mValid = false;
        state.mStaticOnly = false;
        state.mDirty = false;
        state.mMaskTextures.clear();
        state.mStaticCached = false;
        state.mHasDynamic = false;
        state.mSplitRetryTime = 0.f;
    }
}

// Whether depth can be blitted between the atlas and its static copy: both allocated (isComplete
// is false once the render thread found an allocation failed), of one size and one depth format.
// A depth blit between formats that differ is an error, and one from or to a texture without
// storage is undefined.
static bool fs_lls_static_matches(const LLRenderTarget& atlas, const LLRenderTarget& copy)
{
    return atlas.isComplete() && copy.isComplete() && atlas.getDepth() && copy.getDepth() &&
           atlas.getWidth() == copy.getWidth() && atlas.getHeight() == copy.getHeight() &&
           atlas.getFSDepthFormat() == copy.getFSDepthFormat();
}

// The copy of the static casters' depth (renderFace with split), the atlas's size and format.
// Allocated the first time a face keeps its static casters.
bool FSPointLightShadows::ensureStaticAtlas()
{
    if (!mAtlas.isComplete())
    {
        return false;
    }
    if (fs_lls_static_matches(mAtlas, mStaticAtlas))
    {
        return true;
    }
    mStaticAtlas.release();
    if (!mStaticAtlas.allocate(mAtlas.getWidth(), mAtlas.getHeight(), 0, true))
    {
        mStaticAtlas.release();
        return false;
    }
    return fs_lls_static_matches(mAtlas, mStaticAtlas);
}

// The atlas: six faces across, one slot per row, depth only and compared in hardware. Allocated
// in the shadow pass, so the depth stays 24 bit fixed point under reverse-Z as well.
bool FSPointLightShadows::ensureAtlas(U32 rows)
{
    static LLCachedControl<S32> resolution(gSavedSettings, "FSRenderLocalLightShadowResolution", 512);
    U32 res = (U32)llclamp((S32)resolution, 128, 2048);
    const U32 max_size = (U32)llmax(gGLManager.mGLMaxTextureSize, 1024);
    while (res > 128 && (res * FACES > max_size || res * rows > max_size))
    {
        res /= 2;
    }
    if (mAtlas.isComplete() && res == mRes && rows == mRows)
    {
        return true;
    }

    // a new atlas holds nothing yet: every light draws all its faces again
    mAtlas.release();
    mStaticAtlas.release();
    mRes = 0;
    mRows = 0;
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        if (i >= rows)
        {
            clearSlot(i);
            continue;
        }
        LLPointer<LLDrawable> light = mSlots[i].mLight;
        clearSlot(i);
        mSlots[i].mLight = light;
    }

    if (!mAtlas.allocate(res * FACES, res * rows, 0, true))
    {
        mAtlas.release();
        if (!mWarned)
        {
            mWarned = true;
            LL_WARNS("FSLocalLightShadows") << "Could not allocate the " << res * FACES << "x" << res * rows
                                            << " local light shadow atlas, local lights stay unshadowed" << LL_ENDL;
        }
        return false;
    }

    // filtering and compare mode as the projector shadow maps have them: hardware PCF
    gGL.getTexUnit(0)->bind(&mAtlas, true);
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
    gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    mRes = res;
    mRows = rows;
    LL_INFOS("FSLocalLightShadows") << "Local light shadow atlas " << res * FACES << "x" << res * rows << " ("
                                    << rows << " lights, " << res << " texels per face, "
                                    << (res * FACES * res * rows * 4) / (1024 * 1024) << " MB)" << LL_ENDL;
    return true;
}

// The slots follow the lights the last light pass chose (mWanted): a slot whose light dropped out
// fades its shadow away, then takes a wanted light that has none, which fades in once its six
// faces are drawn.
void FSPointLightShadows::assignSlots(U32 count)
{
    const F32 step = llclamp((F32)gFrameIntervalSeconds.value(), 0.f, 0.25f) * FS_LLS_FADE_RATE;
    for (U32 i = count; i < MAX_LIGHTS; ++i)
    {
        clearSlot(i);
    }
    for (U32 i = 0; i < count; ++i)
    {
        Slot& s = mSlots[i];
        if (s.mLight.isNull())
        {
            continue;
        }
        bool wanted = false;
        for (const LLPointer<LLDrawable>& w : mWanted)
        {
            wanted = wanted || (w.notNull() && w == s.mLight);
        }
        if (wanted)
        {
            if (s.mReady)
            {
                s.mFade = llmin(s.mFade + step, 1.f);
            }
            continue;
        }
        s.mFade = llmax(s.mFade - step, 0.f);
        if (s.mFade <= 0.f || !s.mReady)
        {
            logSlot(i, "drops");
            clearSlot(i);
        }
    }
    for (const LLPointer<LLDrawable>& w : mWanted)
    {
        if (w.isNull())
        {
            continue;
        }
        S32 free_slot = -1;
        bool held = false;
        for (U32 i = 0; i < count && !held; ++i)
        {
            held = mSlots[i].mLight == w;
            if (free_slot < 0 && mSlots[i].mLight.isNull())
            {
                free_slot = (S32)i;
            }
        }
        if (!held && free_slot >= 0)
        {
            clearSlot((U32)free_slot);
            mSlots[free_slot].mLight = w;
            logSlot((U32)free_slot, "takes");
        }
    }
}

// Whether a drawn face can be kept as it is: -1 yes, 1 something in it changed, 2 it is old
S32 FSPointLightShadows::facePriority(U32 slot, U32 face) const
{
    const LLPipeline::SpotShadowState& state = mFaces[slot][face];
    if (!state.mValid)
    {
        return 0;
    }
    // what it drew had avatars, moving objects, trees or anything else that can change without a
    // note (fsShadowStateRecord), drawn whole or over its kept static casters, or static geometry
    // in it changed since
    if (!state.mStaticOnly || state.mHasDynamic || state.mDirty || !gPipeline.fsSpotMasksUnchanged(state))
    {
        return 1;
    }
    // an avatar or active object reaching into it now
    const Slot& s = mSlots[slot];
    LLVector4a center;
    center.load3(s.mCenter.mV);
    const F32 cube_reach = s.mFar * 1.7321f;
    const std::vector<LLVector4a>& bounds = gPipeline.fsShadowDynamicBounds();
    for (size_t i = 0; i + 1 < bounds.size(); i += 2)
    {
        LLVector4a delta;
        delta.setSub(bounds[i], center);
        const F32 reach = cube_reach + bounds[i + 1].getLength3().getF32();
        if (delta.dot3(delta).getF32() > reach * reach)
        {
            continue;
        }
        LLVector4a lo;
        LLVector4a hi;
        lo.setSub(bounds[i], bounds[i + 1]);
        hi.setAdd(bounds[i], bounds[i + 1]);
        if (LLPipeline::fsBoxInClip(state.mClip, lo, hi))
        {
            return 1;
        }
    }
    if (gFrameTimeSeconds < state.mRenderTime || gFrameTimeSeconds - state.mRenderTime > FS_LLS_MAX_AGE)
    {
        return 2;
    }
    return -1;
}

void FSPointLightShadows::noteChanges()
{
    if (!gPipeline.mFSPointShadowCacheInUse)
    {
        return;
    }
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        if (mSlots[i].mLight.isNull())
        {
            continue;
        }
        for (LLPipeline::SpotShadowState& state : mFaces[i])
        {
            if (state.mValid)
            {
                gPipeline.fsShadowStateNoteChanges(state);
            }
        }
    }
}

void FSPointLightShadows::clearTile(S32 x, S32 y)
{
    LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LESS);
    LLGLEnable scissor(GL_SCISSOR_TEST);
    glScissor(x, y, (GLsizei)mRes, (GLsizei)mRes);
    glClear(GL_DEPTH_BUFFER_BIT);
}

// One face, into the bound atlas. Whole: every caster, culled and drawn. Split (a face that changed
// while its light didn't): as the projectors' FSPerfSpotShadowSplit does it, the static casters
// (region objects and terrain) are drawn once and their depth kept in mStaticAtlas while the
// change notes leave them be; each time after, that copy goes back in and only the dynamic
// casters (avatars, attachments, animesh, moving objects, trees) are culled and drawn over it. A
// room with dancers then costs the dancers, not the room, every frame they move.
void FSPointLightShadows::renderFace(U32 slot, U32 face, LLCamera& camera, bool split)
{
    Slot& s = mSlots[slot];
    struct RenderingSlot    // FSRenderLocalLightShadowAvatarDistance: the casters' draws ask which light this is
    {
        S32& mSlot;
        ~RenderingSlot() { mSlot = -1; }
    } rendering_slot{ mRenderingSlot };
    mRenderingSlot = (S32)slot;
    const S32 x = (S32)(face * mRes);
    const S32 y = (S32)(slot * mRes);
    const S32 res = (S32)mRes;
    gGLViewport[0] = x;
    gGLViewport[1] = y;
    gGLViewport[2] = res;
    gGLViewport[3] = res;
    glViewport(x, y, (GLsizei)mRes, (GLsizei)mRes);
    // the alpha masked casters' shaders take the target's width as the face's
    LLRenderTarget::sCurResX = mRes;
    LLRenderTarget::sCurResY = mRes;

    const glm::mat4 view = faceView(face, s.mCenter);
    const glm::mat4 proj = faceProjection(s.mNear, s.mFar);
    set_current_modelview(view);
    set_current_projection(proj);

    // Culled to the face's frustum, which ends at the light's reach. The planes come from the face's
    // matrices; the camera also takes the face's own frame and square view, so that nothing reading
    // its axes or field of view sees the main camera's instead.
    const F32* f = FS_LLS_FACE_AXES[face][0];
    const F32* r = FS_LLS_FACE_AXES[face][1];
    const F32* u = FS_LLS_FACE_AXES[face][2];
    LLCamera shadow_cam = camera;
    shadow_cam.setAxes(LLVector3(f[0], f[1], f[2]), LLVector3(-r[0], -r[1], -r[2]), LLVector3(u[0], u[1], u[2]));
    shadow_cam.setOrigin(s.mCenter);
    shadow_cam.setAspect(1.f);
    shadow_cam.setView(2.f * atanf(1.f / projectionScale()));
    shadow_cam.setViewHeightInPixels(res);
    shadow_cam.setNear(s.mNear);
    shadow_cam.setFar(s.mFar);
    LLViewerCamera::updateFrustumPlanes(shadow_cam, false, false, true);

    // Shadow passes run with occlusion culling off: the projector camera IDs only stamp which
    // groups were seen this frame, as for projector slots past the first two
    LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SPOT_SHADOW0 + (face & 1));

    static LLCullResult result;
    LLPipeline::SpotShadowState& state = mFaces[slot][face];
    const glm::mat4 clip = proj * view;
    LLPipeline::RenderSpotLight = s.mLight.get(); // the light's own prim casts nothing

    split = split && state.mValid && gFrameTimeSeconds >= state.mSplitRetryTime;
    if (!split)
    {
        clearTile(x, y);
        gPipeline.renderShadow(view, proj, shadow_cam, result, false);
        gPipeline.fsShadowStateRecord(state, result, clip, true);
        state.mStaticCached = false;
        state.mHasDynamic = false;
    }
    else
    {
        // the kept static casters, while nothing the change notes see changed and not too old
        // (and the copy still matches the atlas: otherwise the face is drawn whole below)
        const bool keep = state.mStaticCached && !state.mDirty && state.mStaticTex == mAtlas.getDepth() &&
                          fs_lls_static_matches(mAtlas, mStaticAtlas) && gFrameTimeSeconds >= state.mRenderTime &&
                          gFrameTimeSeconds - state.mRenderTime <= FS_LLS_MAX_AGE &&
                          gPipeline.fsSpotMasksUnchanged(state);
        if (keep)
        {
            mAtlas.copyContents(mStaticAtlas, x, y, x + res, y + res, x, y, x + res, y + res, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        else
        {
            clearTile(x, y);
            gPipeline.mFSShadowParts = LLPipeline::FS_SHADOW_PARTS_STATIC;
            gPipeline.renderShadow(view, proj, shadow_cam, result, false);
            gPipeline.mFSShadowParts = LLPipeline::FS_SHADOW_PARTS_ALL;
            gPipeline.fsShadowStateRecord(state, result, clip, true);
            state.mStaticCached = false;
            if (state.mStaticOnly && ensureStaticAtlas())
            {
                mStaticAtlas.copyContents(mAtlas, x, y, x + res, y + res, x, y, x + res, y + res, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                state.mStaticCached = true;
                state.mStaticTex = mAtlas.getDepth();
            }
            else
            {
                // can't be kept (an animated alpha mask, say): drawn whole for a while, which
                // costs one cull instead of two
                state.mSplitRetryTime = gFrameTimeSeconds + FS_LLS_SPLIT_RETRY;
            }
        }

        // the dynamic casters over the static depth
        gPipeline.mFSShadowParts = LLPipeline::FS_SHADOW_PARTS_DYNAMIC;
        gPipeline.renderShadow(view, proj, shadow_cam, result, false);
        gPipeline.mFSShadowParts = LLPipeline::FS_SHADOW_PARTS_ALL;
        state.mHasDynamic = result.getVisibleGroupsSize() || result.getVisibleBridgeSize() || result.getVisibleListSize();
    }
    LLPipeline::RenderSpotLight = nullptr;
    state.mValid = true;
}

void FSPointLightShadows::update(LLCamera& camera)
{
    if (gCubeSnapshot)
    {
        return; // the maps are the main view's; a probe capture neither draws nor frees them
    }
    const U32 count = lightCount();
    gPipeline.mFSPointShadowCacheInUse = count > 0;
    if (count == 0)
    {
        if (holdsAnything())
        {
            release();
        }
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    FS_RECORD_BLOCK_TIME(FTM_FS_LOCAL_LIGHT_SHADOWS);

    if (!ensureAtlas(count))
    {
        release();
        return;
    }
    assignSlots(count);

    // the lights whose casters keep their detail (lodDistance)
    mLodCount = 0;
    for (U32 i = 0; i < count; ++i)
    {
        const Slot& s = mSlots[i];
        const LLVOVolume* volume = s.mLight.notNull() && !s.mLight->isDead() ? s.mLight->getVOVolume() : nullptr;
        if (volume)
        {
            mLodCenter[mLodCount] = s.mLight->getPositionAgent();
            mLodReach[mLodCount] = volume->getLightRadius() * 1.5f;
            ++mLodCount;
        }
    }

    static LLCachedControl<S32> faces_per_frame(gSavedSettings, "FSRenderLocalLightShadowFacesPerFrame", 6);
    static LLCachedControl<F32> near_clip(gSavedSettings, "FSRenderLocalLightShadowNearClip", 0.1f);
    // FSRenderLocalLightShadowDebug 3: every face of every light drawn whole every frame (nothing
    // kept); 4: maps once drawn are never drawn again (frozen)
    static LLCachedControl<S32> debug(gSavedSettings, "FSRenderLocalLightShadowDebug", 0);
    const bool redraw_all = debug == 3;
    const bool frozen = debug == 4;
    const U32 budget = redraw_all ? MAX_LIGHTS * FACES : (U32)llclamp((S32)faces_per_frame, 1, (S32)(MAX_LIGHTS * FACES));

    // Whole lights first (new ones, then those that moved or changed size: all six faces from the
    // new position at once, so the cube stays whole), then single faces that changed, the longest
    // waiting first
    struct Job
    {
        U32 mSlot;
        U32 mFace;
        S32 mPriority;
        F32 mTime;
    };
    Job whole[MAX_LIGHTS];
    U32 whole_count = 0;
    Job faces[MAX_LIGHTS * FACES];
    U32 face_count = 0;
    LLVector3 new_center[MAX_LIGHTS];
    F32 new_near[MAX_LIGHTS] = {};
    F32 new_far[MAX_LIGHTS] = {};

    for (U32 i = 0; i < count; ++i)
    {
        Slot& s = mSlots[i];
        if (s.mLight.isNull())
        {
            continue;
        }
        LLVOVolume* volume = s.mLight->getVOVolume();
        if (s.mLight->isDead() || !volume || !volume->getIsLight() || volume->isLightSpotlight() || volume->isAttachment())
        {
            clearSlot(i);
            continue;
        }
        const F32 size = volume->getLightRadius() * 1.5f; // the reach the light passes give it
        new_center[i] = s.mLight->getPositionAgent();
        new_far[i] = size * 1.02f + 0.05f;
        new_near[i] = llclamp((F32)near_clip, 0.02f, new_far[i] * 0.25f);

        const bool moved = dist_vec_squared(new_center[i], s.mCenter) > 1e-6f ||
                           fabsf(new_far[i] - s.mFar) > 1e-4f || fabsf(new_near[i] - s.mNear) > 1e-5f;
        if (frozen && s.mReady)
        {
            continue;
        }
        if (!s.mReady || moved || redraw_all)
        {
            whole[whole_count++] = { i, 0, s.mReady ? 1 : 0, 0.f };
            continue;
        }
        for (U32 f = 0; f < FACES; ++f)
        {
            const S32 priority = facePriority(i, f);
            if (priority >= 0)
            {
                faces[face_count++] = { i, f, priority, mFaces[i][f].mRenderTime };
            }
        }
    }
    if (whole_count == 0 && face_count == 0)
    {
        return; // nothing changed: no draws at all
    }

    std::stable_sort(whole, whole + whole_count, [](const Job& a, const Job& b) { return a.mPriority < b.mPriority; });
    std::stable_sort(faces, faces + face_count, [](const Job& a, const Job& b)
                     { return a.mPriority != b.mPriority ? a.mPriority < b.mPriority : a.mTime < b.mTime; });

    LL_PROFILE_GPU_ZONE("local light shadows");
    S32 saved_viewport[4] = { gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3] };
    const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;

    mAtlas.bindTarget();
    U32 drawn = 0;
    for (U32 j = 0; j < whole_count; ++j)
    {
        // a whole light may run past a budget below six faces, but only as the frame's first draw
        if (drawn > 0 && drawn + FACES > budget)
        {
            break;
        }
        const U32 i = whole[j].mSlot;
        Slot& s = mSlots[i];
        s.mCenter = new_center[i];
        s.mNear = new_near[i];
        s.mFar = new_far[i];
        for (U32 f = 0; f < FACES; ++f)
        {
            renderFace(i, f, camera, false); // moved or new: nothing kept is of use
        }
        s.mReady = true;
        drawn += FACES;
    }
    bool refreshed = false;
    for (U32 j = 0; j < face_count && drawn < budget; ++j)
    {
        // the safety refresh, one face a frame at most: a light's six faces come due together
        if (faces[j].mPriority >= 2)
        {
            if (refreshed)
            {
                break;
            }
            refreshed = true;
        }
        renderFace(faces[j].mSlot, faces[j].mFace, camera, true);
        ++drawn;
    }

    for (U32 i = 0; i < 4; ++i)
    {
        gGLViewport[i] = saved_viewport[i];
    }
    mAtlas.flush();
    LLViewerCamera::sCurCameraID = saved_camera;
}

void FSPointLightShadows::beginLightPass(const LLVector3& camera_origin)
{
    static LLCachedControl<F32> max_distance(gSavedSettings, "FSRenderLocalLightShadowDistance", 32.f);
    static LLCachedControl<F32> strength_setting(gSavedSettings, "FSRenderLocalLightShadowStrength", 1.f);

    const U32 count = gCubeSnapshot ? 0 : lightCount();
    if (!gCubeSnapshot && count == 0 && holdsAnything())
    {
        release(); // turned off while the shadow pass, which frees it otherwise, doesn't run
    }
    mRanking = count > 0;
    mCandidates.clear();
    mCameraOrigin = camera_origin;
    mMaxDistance = llclamp((F32)max_distance, 1.f, 512.f);
    // The lights are weighed from the camera's focus, which orbiting and zooming leave where it is;
    // in mouselook, or with the focus farther off than the shadows reach, from the camera
    mRankOrigin = camera_origin;
    if (mRanking && !gAgentCamera.cameraMouselook())
    {
        const LLVector3 focus = gAgent.getPosAgentFromGlobal(gAgentCamera.getFocusGlobal());
        if (focus.isFinite() && dist_vec(focus, camera_origin) <= mMaxDistance)
        {
            mRankOrigin = focus;
        }
    }
    mShading = count > 0 && mAtlas.isComplete() && mRes > 0;

    const F32 strength = llclamp((F32)strength_setting, 0.f, 1.f);
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        Slot& s = mSlots[i];
        F32 light_strength = 0.f;
        if (mShading && i < mRows && s.mLight.notNull() && s.mReady && s.mFar > s.mNear)
        {
            const F32 dist = llmax(dist_vec(s.mCenter, camera_origin) - s.mFar, 0.f);
            const F32 fade = llclamp((mMaxDistance - dist) / (FS_LLS_DISTANCE_FADE * mMaxDistance), 0.f, 1.f);
            light_strength = strength * s.mFade * fade;
        }
        s.mStrength = light_strength;

        // agent space: the light programs take it to eye space with their own view (fsPointShadowMapF.glsl)
        mUniLight[i * 4 + 0] = s.mCenter.mV[0];
        mUniLight[i * 4 + 1] = s.mCenter.mV[1];
        mUniLight[i * 4 + 2] = s.mCenter.mV[2];
        mUniLight[i * 4 + 3] = light_strength;

        const F32 n = s.mNear;
        const F32 f = s.mFar;
        const bool depth_ok = f > n && n > 0.f;
        mUniDepth[i * 4 + 0] = depth_ok ? (f + n) / (f - n) : 0.f;
        mUniDepth[i * 4 + 1] = depth_ok ? 2.f * f * n / (f - n) : 0.f;
        mUniDepth[i * 4 + 2] = n;
        mUniDepth[i * 4 + 3] = f;
    }
    mUniAtlas[0] = 1.f / (F32)FACES;
    mUniAtlas[1] = mRows > 0 ? 1.f / (F32)mRows : 1.f;
    mUniAtlas[2] = projectionScale();
    mUniAtlas[3] = (F32)llmax(mRes, 1u);
}

// Scored by FSClusteredLights::screenWeight: brightness times the share of the view the light's
// sphere takes
void FSPointLightShadows::rank(LLDrawable* light, const LLVector3& center, F32 size, const LLColor3& color)
{
    if (!mRanking || !light)
    {
        return;
    }
    const LLVOVolume* volume = light->getVOVolume();
    if (!volume || volume->isAttachment())
    {
        return; // worn lights sit inside their wearer, who would shade everything
    }
    static LLCachedControl<F32> near_clip(gSavedSettings, "FSRenderLocalLightShadowNearClip", 0.1f);
    if (size <= llmax((F32)near_clip, 0.02f) * 4.f)
    {
        return; // too small to cast anything worth a map
    }
    if (llmax(dist_vec(center, mCameraOrigin) - size, 0.f) > mMaxDistance)
    {
        return;
    }
    F32 score = FSClusteredLights::screenWeight(dist_vec_squared(center, mRankOrigin), size, color);
    if (!(score > 0.f))
    {
        return;
    }
    for (const Slot& s : mSlots)
    {
        if (s.mLight.get() == light)
        {
            score *= FS_LLS_HOLD_BONUS;
            break;
        }
    }
    mCandidates.emplace_back(score, light);
}

void FSPointLightShadows::endLightPass()
{
    if (!mRanking)
    {
        return;
    }
    mRanking = false;
    const U32 count = llmin(lightCount(), (U32)mCandidates.size());
    // strongest first, equal scores in the stock loop's order (nearest first)
    std::stable_sort(mCandidates.begin(), mCandidates.end(),
                     [](const std::pair<F32, LLDrawable*>& a, const std::pair<F32, LLDrawable*>& b) { return a.first > b.first; });
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        mWanted[i] = i < count ? mCandidates[i].second : nullptr;
    }

    // FSRenderLocalLightShadowDebug: each time the choice changes, the lights ranked and chosen
    static LLCachedControl<S32> debug(gSavedSettings, "FSRenderLocalLightShadowDebug", 0);
    bool changed = mRanked != (U32)mCandidates.size();
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        changed = changed || mLoggedWanted[i] != mWanted[i].get();
    }
    if (debug > 0 && changed)
    {
        LL_INFOS("FSLocalLightShadows") << mCandidates.size() << " point lights ranked from " << mRankOrigin << " (camera at "
                                        << mCameraOrigin << "), maps for the " << count << " strongest:" << LL_ENDL;
        for (size_t i = 0; i < mCandidates.size(); ++i)
        {
            const LLDrawable* light = mCandidates[i].second;
            const LLVOVolume* volume = light ? light->getVOVolume() : nullptr;
            LL_INFOS("FSLocalLightShadows") << (i < count ? "  map  " : "  none ") << (volume ? volume->getID() : LLUUID::null)
                                            << " at " << (light ? light->getPositionAgent() : LLVector3::zero)
                                            << " radius " << (volume ? volume->getLightRadius() : 0.f)
                                            << " weight " << mCandidates[i].first << LL_ENDL;
        }
    }
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        mLoggedWanted[i] = mWanted[i].get();
    }
    mRanked = (U32)mCandidates.size();
    mCandidates.clear();
}

// FSRenderLocalLightShadowDebug: a slot taking or dropping its light
void FSPointLightShadows::logSlot(U32 slot, const char* what) const
{
    static LLCachedControl<S32> debug(gSavedSettings, "FSRenderLocalLightShadowDebug", 0);
    if (debug <= 0 || slot >= MAX_LIGHTS || mSlots[slot].mLight.isNull())
    {
        return;
    }
    const LLDrawable* light = mSlots[slot].mLight.get();
    const LLVOVolume* volume = light->getVOVolume();
    LL_INFOS("FSLocalLightShadows") << "Slot " << slot << " " << what << " light " << (volume ? volume->getID() : LLUUID::null)
                                    << " at " << light->getPositionAgent() << LL_ENDL;
}

bool FSPointLightShadows::renderingLight(LLVector3& center) const
{
    if (mRenderingSlot < 0 || mRenderingSlot >= (S32)MAX_LIGHTS)
    {
        return false;
    }
    center = mSlots[mRenderingSlot].mCenter;
    return true;
}

F32 FSPointLightShadows::lodDistance(const LLVector3& center, F32 radius, F32 distance) const
{
    for (U32 i = 0; i < mLodCount; ++i)
    {
        const F32 d = dist_vec(center, mLodCenter[i]);
        if (d - radius < mLodReach[i])
        {
            distance = llmin(distance, llmax(d, 0.01f));
        }
    }
    return distance;
}

void FSPointLightShadows::drawDebugAtlas(LLRenderTarget* dst, LLVertexBuffer* screen_tri)
{
    static LLCachedControl<S32> debug(gSavedSettings, "FSRenderLocalLightShadowDebug", 0);
    static LLStaticHashedString s_depth("fs_lls_depth");
    static LLStaticHashedString s_info("fs_lls_info");
    LLGLSLShader& shader = gFSPointShadowAtlasProgram;
    if (debug < 2 || !dst || !screen_tri || !mAtlas.isComplete() || mRows == 0 || !shader.isComplete())
    {
        return;
    }
    LL_PROFILE_GPU_ZONE("local light shadow atlas debug");

    // the lower left, half the target wide (or less, so it is at most half as high)
    S32 width = (S32)dst->getWidth() / 2;
    S32 height = width * (S32)mRows / (S32)FACES;
    if (height > (S32)dst->getHeight() / 2)
    {
        height = (S32)dst->getHeight() / 2;
        width = height * (S32)FACES / (S32)mRows;
    }
    if (width < 16 || height < 16)
    {
        return;
    }

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    dst->bindTarget();
    glViewport(8, 8, width, height);
    shader.bind();
    const S32 channel = shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mAtlas, true, LLTexUnit::TFO_BILINEAR); // the filter the compare uses
    if (channel >= 0)
    {
        // raw depth while drawn here, the hardware compare again after
        gGL.getTexUnit(channel)->activate();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }
    shader.uniform4fv(s_depth, MAX_LIGHTS, mUniDepth);
    shader.uniform4f(s_info, (F32)FACES, (F32)mRows, 0.f, 0.f);
    screen_tri->setBuffer();
    screen_tri->drawArrays(LLRender::TRIANGLES, 0, 3);
    if (channel >= 0)
    {
        gGL.getTexUnit(channel)->activate();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        shader.unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
        gGL.getTexUnit(0)->activate();
    }
    shader.unbind();
    dst->flush();
}

S32 FSPointLightShadows::slotOf(const LLDrawable* light) const
{
    if (!mShading || !light)
    {
        return -1;
    }
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        if (mSlots[i].mLight.get() == light)
        {
            return mSlots[i].mStrength > 0.f ? (S32)i : -1;
        }
    }
    return -1;
}

bool FSPointLightShadows::bind(LLGLSLShader& shader, S32 unit)
{
    static LLStaticHashedString s_map("fs_lls_map");
    static LLStaticHashedString s_light("fs_lls_light");
    static LLStaticHashedString s_depth("fs_lls_depth");
    static LLStaticHashedString s_atlas("fs_lls_atlas");
    static LLStaticHashedString s_debug("fs_lls_debug");
    static LLCachedControl<S32> debug(gSavedSettings, "FSRenderLocalLightShadowDebug", 0);

    if (!programSamples(shader))
    {
        return false;
    }
    const S32 units = llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);
    if (unit < 0 || unit >= units)
    {
        LL_WARNS_ONCE("FSLocalLightShadows") << "No texture unit left in " << shader.mName << " for the local light shadow maps" << LL_ENDL;
        // every slot without strength: whatever slot the program's uniforms name, it samples nothing
        static const F32 none[MAX_LIGHTS * 4] = {};
        shader.uniform4fv(s_light, MAX_LIGHTS, none);
        return false;
    }

    // Always a unit of its own, a depth texture on it when there is one: a shadow sampler left on
    // unit 0 would share it with a colour sampler, which fails the draw
    shader.uniform1i(s_map, unit);
    if (mAtlas.isComplete())
    {
        gGL.getTexUnit(unit)->bind(&mAtlas, true);
    }
    else
    {
        gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
    }
    mBoundUnit = unit;

    shader.uniform4fv(s_light, MAX_LIGHTS, mUniLight);
    shader.uniform4fv(s_depth, MAX_LIGHTS, mUniDepth);
    shader.uniform4fv(s_atlas, 1, mUniAtlas);
    shader.uniform1i(s_debug, llclamp((S32)debug, 0, 6));
    return true;
}

void FSPointLightShadows::unbind()
{
    if (mBoundUnit >= 0)
    {
        gGL.getTexUnit(mBoundUnit)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(0)->activate();
        mBoundUnit = -1;
    }
}

void FSPointLightShadows::forget(const LLDrawable* light)
{
    if (!light)
    {
        return;
    }
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        if (mSlots[i].mLight.get() == light)
        {
            clearSlot(i);
        }
        if (mWanted[i].get() == light)
        {
            mWanted[i] = nullptr;
        }
    }
    mCandidates.erase(std::remove_if(mCandidates.begin(), mCandidates.end(),
                                     [light](const std::pair<F32, LLDrawable*>& c) { return c.second == light; }),
                      mCandidates.end());
}

void FSPointLightShadows::release()
{
    mAtlas.release();
    mStaticAtlas.release();
    mRes = 0;
    mRows = 0;
    for (U32 i = 0; i < MAX_LIGHTS; ++i)
    {
        clearSlot(i);
        mWanted[i] = nullptr;
    }
    mCandidates.clear();
    mCandidates.shrink_to_fit();
    mLodCount = 0;
    mShading = false;
    mRanking = false;
    gPipeline.mFSPointShadowCacheInUse = false;
}
