/**
 * @file fspointlightshadows.h
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

#ifndef FS_POINTLIGHTSHADOWS_H
#define FS_POINTLIGHTSHADOWS_H

#include "llpointer.h"
#include "llrendertarget.h"
#include "pipeline.h"
#include "v3color.h"
#include "v3math.h"

#include "glm/mat4x4.hpp"

#include <utility>
#include <vector>

class LLCamera;
class LLDrawable;
class LLGLSLShader;
class LLVertexBuffer;

// Real shadows from the local point lights that matter most on screen (FSRenderLocalLightShadows,
// 0 to 8 lights). Each such light gets a slot: a cube of six depth maps, the 90 degree views from
// the light along the agent axes, drawn with renderShadow like the projector shadows (the same
// casters: opaque, alpha masked, avatars and rigged meshes; the light's own prim left out), and
// sampled by whichever local light pass draws that light (fsPointShadowMapF.glsl): the clustered
// lights, the stock light volumes or the stock full screen passes.
//
// Why cube maps: every face is an ordinary perspective view, so the shadow passes, their culling
// and the GPU-driven shadow draws are reused as they are, big prims cast exactly (a dual-paraboloid
// map needs finely tessellated casters to bend their edges), and the texel density is even all
// round. Six culls and draws per light instead of two, but a face is only drawn again when
// something in it changed, so a still scene costs no draws at all.
//
// All slots share one depth texture, an atlas of six faces across and one row per slot, so the
// light programs need a single texture unit for any number of lights. Faces are drawn slightly
// wider than 90 degrees: the guard band around each holds what lies just over its edge, so the
// fixed 3x3 filter never reads outside its face and the seams between faces don't show.
//
// Choosing the lights: the light pass ranks the nearby point lights, in view or not (turning the
// camera must not hand the maps to other lights), by FSClusteredLights::screenWeight (brightness
// times the share of the view the light's sphere would take) seen from the camera's focus, the
// point the camera orbits, not from the camera: orbiting or zooming must not hand the maps to
// other lights either (a light that loses its map shines through walls). Worn lights and lights
// farther than FSRenderLocalLightShadowDistance from the camera are left out; the lights holding
// a slot win ties by half again, so slots don't swap back and forth. Next frame's
// shadow pass hands the slots out: a light that drops out fades its shadow away before its slot
// is reused, a new one fades in once all six faces are drawn.
//
// Keeping the maps: a face is kept while nothing it shows changed, by the same notes as the
// projector shadow cache (FSPerfSpotShadowCache): static geometry changes and moves noted in its
// frustum, avatars and active objects reaching into it, alpha masks still loading, and a refresh
// every few seconds as a safety net. A light that moves or changes size draws all six faces again.
// At most FSRenderLocalLightShadowFacesPerFrame faces are drawn per frame, new lights first, then
// the faces that changed longest ago. Nothing is drawn during reflection probe captures, and the
// atlas is freed when the setting is 0.
class FSPointLightShadows
{
public:
    static constexpr U32 MAX_LIGHTS = 8;
    static constexpr U32 FACES = 6;

    // FSRenderLocalLightShadows above 0 and shadows on, read when the shaders are built: the
    // local light programs get the map lookup (FS_POINT_SHADOW_MAP)
    static bool wantedByShaders();

    // How many lights get a map now: 0 when off, or no local light program can sample one
    U32 lightCount() const;

    // generateSunShadow, after the projectors, with the shadow render types and the standard depth
    // convention: hands the slots to the lights ranked by the last light pass and draws the faces
    // that need it. Does nothing during reflection probe captures.
    void update(LLCamera& camera);

    // fsSpotCacheBeginFrame, before the change notes are cleared: marks the faces they reach
    void noteChanges();

    // renderDeferredLighting, around the stock local light loop. beginLightPass sets what the light
    // programs sample in this pass (nothing in a reflection probe face) and, in the main view,
    // starts ranking; rank takes each point light the loop draws (agent space centre, the stock
    // size of radius * 1.5, linear colour); endLightPass keeps the best for the next shadow pass.
    void beginLightPass(const LLVector3& camera_origin);
    void rank(LLDrawable* light, const LLVector3& center, F32 size, const LLColor3& color);
    void endLightPass();

    // The light's slot in this light pass, -1 when it has no map to sample
    S32 slotOf(const LLDrawable* light) const;

    // After binding a local light program: the atlas on texture unit `unit` (the first one past the
    // program's own) and the uniforms of fsPointShadowMapF.glsl. False, binding nothing, when the
    // program has no map lookup or there is no such unit.
    static bool programSamples(const LLGLSLShader& shader);
    bool bind(LLGLSLShader& shader, S32 unit);
    void unbind();

    void forget(const LLDrawable* light);   // LLPipeline::unlinkDrawable

    // LLVOVolume::calcLOD: the distance an object's level of detail is chosen for. Within the reach
    // of a light with a map, no farther than the object is from that light: the map is drawn with
    // the casters' current detail, so detail chosen by the camera's distance alone would change the
    // shadows (a fitting's low detail model letting light out) as the camera moves.
    F32 lodDistance(const LLVector3& center, F32 radius, F32 distance) const;

    // FSRenderLocalLightShadowDebug 2 and up: the atlas drawn into the lower left of dst, one row
    // per slot, six faces across, nearer casters brighter
    void drawDebugAtlas(LLRenderTarget* dst, LLVertexBuffer* screen_tri);
    void release();                         // frees the atlas and the slots

private:
    struct Slot
    {
        LLPointer<LLDrawable> mLight;
        bool        mReady = false;         // all six faces drawn for this light
        F32         mFade = 0.f;            // shadow strength fading in and out, 0-1
        LLVector3   mCenter;                // agent space position the faces were drawn from
        F32         mNear = 0.f;
        F32         mFar = 0.f;
        F32         mStrength = 0.f;        // this light pass: setting, fade and distance fade
    };

    bool holdsAnything() const;             // the atlas, a slot's light or a wanted light
    bool ensureAtlas(U32 rows);
    void assignSlots(U32 count);
    void clearSlot(U32 slot);
    // Whether a drawn face can be kept: -1 kept, 1 something in it changed, 2 due for a refresh
    S32 facePriority(U32 slot, U32 face) const;
    void renderFace(U32 slot, U32 face, LLCamera& camera, bool split);
    void clearTile(S32 x, S32 y);
    bool ensureStaticAtlas();
    glm::mat4 faceView(U32 face, const LLVector3& eye) const;
    glm::mat4 faceProjection(F32 znear, F32 zfar) const;
    F32 projectionScale() const;            // 1 / tan of a face's half angle, guard band included

    Slot mSlots[MAX_LIGHTS];
    // What each face's last render saw, for keeping it (the projector shadow cache's record);
    // mValid: drawn for the slot's current light
    LLPipeline::SpotShadowState mFaces[MAX_LIGHTS][FACES];

    LLRenderTarget mAtlas;
    LLRenderTarget mStaticAtlas;            // static casters' depth of the faces drawn split
    U32 mRes = 0;                           // texels per face
    U32 mRows = 0;                          // slots the atlas holds
    bool mWarned = false;

    // Ranking of this light pass, and the lights it chose for the next shadow pass
    bool mRanking = false;
    LLVector3 mCameraOrigin;
    LLVector3 mRankOrigin;                  // where the lights are weighed from: the camera's focus
    F32 mMaxDistance = 0.f;
    std::vector<std::pair<F32, LLDrawable*>> mCandidates;
    LLPointer<LLDrawable> mWanted[MAX_LIGHTS];
    // FSRenderLocalLightShadowDebug: the choice last logged, and how many lights were ranked
    const LLDrawable* mLoggedWanted[MAX_LIGHTS] = {};
    U32 mRanked = 0;
    void logSlot(U32 slot, const char* what) const;

    // The lights with a slot as lodDistance sees them: agent space centre and reach
    LLVector3 mLodCenter[MAX_LIGHTS];
    F32 mLodReach[MAX_LIGHTS] = {};
    U32 mLodCount = 0;

    // What the light programs sample in this light pass
    bool mShading = false;
    F32 mUniLight[MAX_LIGHTS * 4] = {};
    F32 mUniDepth[MAX_LIGHTS * 4] = {};
    F32 mUniAtlas[4] = {};
    S32 mBoundUnit = -1;
};

extern FSPointLightShadows gFSPointLightShadows;

#endif // FS_POINTLIGHTSHADOWS_H
