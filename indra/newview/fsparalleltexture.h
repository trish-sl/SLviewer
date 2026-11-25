/**
 * @file fsparalleltexture.h
 * @brief Texture decode priority face pass on worker threads (FSPerfParallelTexturePriority)
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

#ifndef FS_PARALLELTEXTURE_H
#define FS_PARALLELTEXTURE_H

#include "llpointer.h"
#include "stdtypes.h"

#include <unordered_set>
#include <vector>

class LLFace;
class LLTextureEntry;
class LLViewerObject;
class LLViewerFetchedTexture;

// Two corrections both face loops (updateImageDecodePriority and FSParallelTexture's) make to a face's
// virtual size. Read on the main thread once per sweep or call (current()); the checks only read.
//  - FSTexturePriorityHiddenFaces: a face in a group the main camera's occlusion test has found hidden for
//    FSTexturePriorityHiddenGrace seconds counts as off screen (its textures take the texture bias, and
//    get no camera boost), like a face outside the view. The grace keeps a texture sharp behind something
//    passing in front of it, and the results being a frame or more late; a face that comes back into view
//    counts as on screen again at once.
//  - FSTexturePriorityGLTFScale: on a face with a GLTF material, its PBR textures use the repeats of the
//    material's texture transform for their channel (which is what draws them), not the face's legacy
//    texture repeats (which GLTF materials don't use).
//  - FSTextureOccludedDiscard: a face in the view whose group the occlusion test has found hidden for the
//    same grace counts at a quarter of its virtual size, one texture level less. A texture keeps the largest
//    size of its faces, so only one whose every face in the view is hidden that way is reduced; such a texture
//    is also reconsidered at every sweep (mOnlyOccluded), as off screen ones are while the bias is up, or the
//    size it had before would hold. A face found visible again counts in full at once (no grace that way).
//  - FSTextureBiasRamp: an in view face takes the texture bias in proportion to how far the bias is above 1
//    (less the face's importance to the camera), instead of all of it once the bias crosses 1.9 (2.4 for the
//    most important faces). A bias drifting across 1.9 no longer flips the whole mid and far scene between
//    full and quarter resolution; each texture steps by its own size as the bias moves.
//  - FSTextureMaterialMapReduce: a normal, specular or metallic-roughness map on a face further than
//    FSTextureMaterialMapDistance from the camera is wanted one level below its colour texture. Only unboosted
//    LOD textures on world objects (not HUDs); not on top of FSTextureOccludedDiscard's level.
struct FSTextureFaceBias
{
    // FSTextureOccludedDiscard: an occluded face's virtual size is multiplied by this, one discard level
    static constexpr F32 OCCLUDED_VSIZE_SCALE = 0.25f;

    bool mHidden = false;
    bool mOccludedDiscard = false;
    bool mGLTFScale = false;
    F32 mGrace = 0.f;
    F64 mNow = 0.0;
    F32 mAvatarFloor = 0.f; // FSTextureAvatarSmallParts: virtual size floor, 0 when off
    bool mBiasRamp = false; // FSTextureBiasRamp
    F32 mDistanceScale = 0.f; // FSTextureBiasDistanceScale: per metre, 0 when off
    F32 mCameraBoostCap = 0.f; // FSTextureCameraBoostMaxLevels as a multiplier (4 for one level), 0 when off
    F32 mMapReduceDistance = -1.f; // FSTextureMaterialMapReduce: metres, below 0 when off
    bool mKeepSelf = false; // FSRenderKeepSelfLoaded

    static FSTextureFaceBias current();
    // face->mInFrustum, unless hidden as above. FSRenderKeepSelfLoaded: always for a face of your own avatar or
    // its world attachments (keepSelf()).
    bool inView(const LLFace* face) const;
    // FSTextureOccludedDiscard: in the frustum, but in a group hidden for the grace. Never a keepSelf() face.
    bool occluded(const LLFace* face) const;
    // FSRenderKeepSelfLoaded: a face of your own avatar or of one of its world attachments. Its textures are
    // prioritised as if it were in view, unoccluded and of the highest importance to the camera (importance()),
    // wherever the camera looks, so they are at full size the moment you see yourself again. They still give way
    // to real video memory shortage like anything in view.
    bool keepSelf(const LLFace* face) const;
    // The face's importance to the camera for the corrections below: importance (face->mImportanceToCamera), or 1
    // for a keepSelf() face
    F32 importance(const LLFace* face, F32 importance) const;
    // What the virtual size of a face in view is multiplied by for its importance to the camera: importance x
    // boost (TextureCameraBoost), at least 1, and with FSTextureCameraBoostMaxLevels at most that many
    // discard levels (4 per level), so a texture inside a large object's bounding sphere (importance 1) isn't
    // loaded 8 times the pixels the screen shows
    F32 cameraBoost(F32 importance, F32 boost) const;
    // FSTextureAvatarSmallParts: the virtual size of an in view face of a world attachment, raised to the
    // floor, so small parts (eyes, lips, nails) load early and stay loaded instead of going by their tiny
    // pixel area. Reads only flags of objp, safe on the priority workers.
    F32 avatarFloor(const LLViewerObject* objp, F32 vsize) const;
    // the smaller of the face's texture repeats in S and T for a texture in channel (LLRender::eTexIndex)
    F32 repeats(const LLTextureEntry* te, U32 channel) const;
    // The scale a face's virtual size is divided by: its repeats (above), or the frame size of a texture
    // animation that shows one frame at a time, squared and clamped to [scale_min, scale_max]
    // (TextureScaleMinAreaFactor, TextureScaleMaxAreaFactor). te may be null (scale 1).
    F32 scale(const LLViewerObject* objp, const LLTextureEntry* te, S32 te_offset, U32 channel, F32 scale_min,
              F32 scale_max) const;
    // The texture bias as the divisor of an off screen face's virtual size, for a texture whose lowest level
    // is max_discard. FSTextureMaxDiscardBias: each step of desired_bias above 4 lets a texture go one level
    // deeper (a 2048 one 512 -> 256 -> 128), as below 4 the per texture limit already stops it at the same size.
    static F32 vsizeBias(F32 max_discard, F32 desired_bias);
    // The bias level vsizeBias() turns into a divisor (1 or more)
    static F32 biasLevel(F32 max_discard, F32 desired_bias);
    // What a face's virtual size is divided by for the texture bias: vsize_bias (vsizeBias()) for a face out of
    // view; for one in view, vsize_bias once desired_bias is over 1.9 + importance / 2, or 1 below that (the
    // stock rule), or with FSTextureBiasRamp a smooth part of it (bias_level is biasLevel()), and at least
    // (desired_bias - 1) * (1 + distance * FSTextureBiasDistanceScale) as the Alchemy viewer does, so far faces
    // give memory back first; never below the texture's own floor (max_discard as for vsizeBias()).
    // distance is the face's distance from the camera in metres (LLFace::getFSCameraDistance()).
    F32 faceDivisor(bool in_view, F32 importance, F32 vsize_bias, F32 bias_level, F32 desired_bias, F32 distance,
                    F32 max_discard) const;
    // FSTextureMaterialMapReduce: what the virtual size of a face showing the texture in channel is multiplied
    // by: OCCLUDED_VSIZE_SCALE for a material map far from the camera, else 1. reducible: the texture is an
    // unboosted LOD texture. occluded: the face already takes FSTextureOccludedDiscard's level.
    F32 materialMapScale(U32 channel, const LLFace* face, const LLViewerObject* objp, bool reducible,
                         bool occluded) const;
};

// What the face loop of LLViewerTextureList::updateImageDecodePriority produces for one texture:
// the largest biased face virtual size, whether any face was in the frustum, and the number of
// faces counted (before the "too many faces" check).
struct FSTexturePriorityFaces
{
    F32  mMaxVSize = 0.f;
    U32  mFaceCount = 0;
    bool mOnScreen = false;
    bool mOnlyOccluded = false; // FSTextureOccludedDiscard: faces in the frustum, all of them occluded
};

// Parallel face pass for the regular texture sweep in updateImagesFetchTextures.
//
// prepare() runs, before the sweep, the expensive and side-effect-free part of
// updateImageDecodePriority for every texture the sweep will visit: LLFace::calcPixelArea for the
// stale faces (each face once, on one thread) and the per-texture reduction over the face list.
// The sweep then calls updateImageDecodePriority as before, in the same order and under the same
// time budget, with the precomputed result: everything that changes textures, the fetcher or the
// list (updateSceneTextureStats, processTextureStats, updateFetch, deleteImage, raw image
// cleanup, spot light priorities) stays on the main thread.
class FSParallelTexture
{
public:
    typedef std::vector<LLPointer<LLViewerFetchedTexture> > entries_t;
    typedef std::unordered_set<LLViewerFetchedTexture*> skip_set_t;

    // Returns true when the face pass ran in parallel for this sweep. False when the setting is
    // off or the sweep is too small; then result() returns nullptr for every entry.
    static bool prepare(const entries_t& entries, const skip_set_t& skip);

    // Precomputed result for entries[index], or nullptr when it has none (not eligible when
    // prepare() ran: the caller then runs the serial face loop).
    static const FSTexturePriorityFaces* result(size_t index, const LLViewerFetchedTexture* imagep);

    // FSPerfParallelTexturePriorityVerify: the caller runs the serial face loop (which applies)
    // and hands both results here.
    static bool verifying();
    static void verify(const LLViewerFetchedTexture* imagep, const FSTexturePriorityFaces& parallel,
                       const FSTexturePriorityFaces& serial);

    // End of the sweep: drop the per-frame state.
    static void finish();

private:
    // Helpers that read private LLFace state (LLFace befriends this class).
    struct Params;
    static void reduceFaces(LLViewerFetchedTexture* imagep, const Params& p, FSTexturePriorityFaces& out);
    static void verifyFaces();
};

#endif // FS_PARALLELTEXTURE_H
