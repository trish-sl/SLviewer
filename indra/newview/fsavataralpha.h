/**
 * @file fsavataralpha.h
 * @brief Cheap lighting for the blended alpha of distant avatars (FSRenderCheapAvatarAlpha), and the share
 *        of avatar alpha in the GPU timings log
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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

#ifndef FS_AVATARALPHA_H
#define FS_AVATARALPHA_H

#include "stdtypes.h"
#include "llpointer.h"

#include <vector>

class LLDrawInfo;
class LLSpatialGroup;
class LLVector4a;
class LLTextureEntry;
class LLViewerObject;
class LLViewerTexture;
class LLVOAvatar;

// Hair, lace, sheer clothing and PBR blend on avatars are drawn by the alpha pool (LLDrawPoolAlpha::renderAlpha)
// with full forward lighting per pixel: a filtered sun shadow, the walk over the reflection probes, seven local
// lights and fog. With FSRenderCheapAvatarAlpha on, the draws of an avatar farther from the camera than
// FSRenderCheapAvatarAlphaDistance get the cheap lighting of particles instead (FSParticleRender::CHEAP in the
// per draw uniform fs_particle, read by class2 alphaF, class2 pbralphaF and class3 materialF): one sun shadow
// tap, the sky probe alone, the four nearest lights; fog as before. Never the own avatar, HUDs, impostor bakes
// or animesh. The distance is taken once per avatar per frame.
// With FSRenderGPUTimings on, avatar alpha is the GPU zone "alpha avatars" (each rigged alpha pass, and the
// groups of non-rigged attachments, up to a cap per frame), and a #FSAvatarAlpha line gives its draws.
// Main thread only, like the rest of the render pipeline.
namespace FSAvatarAlpha
{
    // At the start of a renderAlpha call: whether its draws may be lit cheaply at all (FSRenderCheapAvatarAlpha
    // or FSRenderCheapAlphaDistance on, not HUDs, not an impostor bake). False costs the pass nothing more.
    bool beginPass();

    // FSRenderCheapAlphaDistance: the fs_particle flags of the draws of a group of in-world alpha (not an avatar's,
    // not particles) in a pass beginPass allowed: CHEAP when the group is farther from the camera than the setting,
    // else 0. ext: the group's (or its bridge's) extents. With the stats on and log_ok (the main view's colour
    // pass), the group also goes into the log of the biggest in-world alpha groups on screen.
    S32 sceneGroup(LLSpatialGroup* group, const LLVector4a* ext, const std::vector<LLPointer<LLDrawInfo>>& draws, bool log_ok);

    // The wearer of a group of non-rigged attachments (PARTITION_AVATAR), or nullptr
    const LLVOAvatar* groupAvatar(LLSpatialGroup* group);

    // The fs_particle flags of a draw of this avatar's alpha in a pass beginPass allowed: CHEAP when it is
    // another avatar beyond the distance, else 0
    S32 flags(const LLVOAvatar* avatar);

    // At the end of a renderAlpha call: whether any of its draws got a non-zero flag, so the programs are reset
    // (FSParticleRender::resetAll) before anything else draws with them
    bool endPass();

    // Stats (no-ops while off): whether they are on, and an avatar alpha draw of the main view
    bool statsOn();
    void noteDraw(const LLVOAvatar* avatar, S32 flags);

    // GPU zone of a non-rigged attachment group: timings on, and under the per frame cap
    bool openZone();

    // Main thread, every frame (llappviewer.cpp, with FSGPUTimers): stats on or off, logged every interval
    void frame(bool enabled, F32 interval_seconds);

    // FSRenderAlphaRiggedNoDiscard: whether a rigged alpha draw of the legacy alpha program could never fail its
    // alpha test, so the program built without the discards (gFSDeferredSkinnedAlphaNoDiscardProgram) draws it the
    // same: every texture it samples is loaded and has no alpha channel, and every face colour alpha of the batch
    // is above the pass's minimum alpha (LLDrawInfo::mFSTintAboveMin). Not for particles, HUDs or impostors (the
    // caller's pass decides those).
    bool noDiscardDraw(const LLDrawInfo& params);

    // FSRenderAvatarTintOpaqueAlpha (0: off): whether a face of an attachment whose texture is loaded with no alpha
    // channel and whose face colour alpha is at least the setting (and below 0.999, where it is opaque anyway) is
    // to be drawn opaque, in the deferred pass, as if its face colour alpha were 1. Decided where the face's pool
    // and pass are (LLPipeline::getPoolTypeFromTE's callers, LLVolumeGeometryManager::genDrawInfo and
    // registerFace); LLVOVolume::setTEColor rebuilds when a colour change crosses the threshold.
    bool tintOpaque(const LLTextureEntry* te, const LLViewerTexture* tex, const LLViewerObject* vobj);
    F32 tintOpaqueThreshold(); // the setting clamped to 0.5-0.999, or 0 for off
}

#endif // FS_AVATARALPHA_H
