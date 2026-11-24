/**
 * @file fsavataralpha.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsavataralpha.h"

#include "fsparticlerender.h"
#include "llagentcamera.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "lltextureentry.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewertexture.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <algorithm>
#include <unordered_map>

extern bool gCubeSnapshot;

namespace
{
    // Each avatar's flags, worked out at its first alpha draw of the frame (the camera to avatar distance
    // once per avatar per frame, as the probe, mirror and main passes all ask). Cleared when the frame changes;
    // avatars are only deleted outside rendering, so a pointer can't come back as another avatar meanwhile.
    std::unordered_map<const LLVOAvatar*, S32> sCache;
    U32 sCacheFrame = 0;
    const LLVOAvatar* sLastAvatar = nullptr;    // consecutive draws are mostly one avatar's
    S32 sLastFlags = 0;
    // FSRenderCheapAvatarAlphaCrowd: the other avatars drawn in full this frame (not you, animesh, impostors, muted
    // or out of view), counted once a frame with the cache
    S32 sCrowd = 0;
    F32 sScreenPixels = 0.f;                    // FSRenderCheapAvatarAlphaCoverage: the main view's pixels

    S32 count_crowd()
    {
        S32 crowd = 0;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character); // isVisuallyMuted and isImpostor aren't const
            if (avatar && !avatar->isDead() && !avatar->isSelf() && !avatar->isControlAvatar() && !avatar->isVisuallyMuted() &&
                !avatar->isImpostor() && avatar->isVisible())
            {
                ++crowd;
            }
        }
        return crowd;
    }

    bool sFlaggedThisPass = false;

    // The rigged passes are a zone each; the non-rigged attachments are interleaved with the rest of the
    // alpha, so their groups are, up to this many a frame (FSGPUTimers times 1024 zones a frame in all,
    // particles take up to 256)
    constexpr U32 MAX_ZONES_PER_FRAME = 256;
    U32 sZonesThisFrame = 0;

    bool sStatsOn = false;
    std::unordered_map<const LLVOAvatar*, bool> sSeen;  // this frame: avatars with alpha drawn, cheaply or not

    // FSRenderCheapAlphaDistance: the in-world alpha groups of this frame's main view, for the log of the biggest
    // on screen (the last frame's top entries are what gets logged)
    struct SceneGroup
    {
        F32 mArea = 0.f;        // share of the view, 0-1
        F32 mDistance = 0.f;
        U32 mDraws = 0;
        U32 mTris = 0;
        bool mCheap = false;
        bool mBridge = false;
        LLUUID mId;
        LLVector3 mPosition;
    };
    constexpr size_t SCENE_GROUPS_LOGGED = 8;
    std::vector<SceneGroup> sSceneGroups;       // this frame
    std::vector<SceneGroup> sSceneGroupsLast;   // the last frame that had any, the top entries by area
    U32 sSceneDraws = 0;                        // summed over the interval, main view
    U32 sSceneCheapDraws = 0;

    F32 scene_distance()
    {
        static LLCachedControl<F32> distance(gSavedSettings, "FSRenderCheapAlphaDistance", 0.f);
        return distance > 0.f ? llmax((F32)distance, 1.f) : 0.f;
    }

    // summed over the interval
    U32 sFrames = 0;
    U64 sDraws = 0;
    U64 sCheapDraws = 0;
    U64 sAvatars = 0;
    U64 sCheapAvatars = 0;
    U64 sUntimedGroups = 0;

    bool main_view()
    {
        return !gCubeSnapshot && !LLPipeline::sReflectionRender && !LLPipeline::sImpostorRender &&
            !LLPipeline::sRenderingHUDs && gPipeline.mRT == &gPipeline.mMainRT;
    }

    void end_frame()
    {
        for (const auto& seen : sSeen)
        {
            ++sAvatars;
            sCheapAvatars += seen.second ? 1 : 0;
        }
        sSeen.clear();
        if (!sSceneGroups.empty())
        {
            std::sort(sSceneGroups.begin(), sSceneGroups.end(), [](const SceneGroup& a, const SceneGroup& b) { return a.mArea > b.mArea; });
            sSceneGroups.resize(llmin(sSceneGroups.size(), SCENE_GROUPS_LOGGED));
            sSceneGroupsLast.swap(sSceneGroups);
            sSceneGroups.clear();
        }
    }

    void log_stats()
    {
        if (!sFrames)
        {
            return;
        }
        static LLCachedControl<bool> cheap(gSavedSettings, "FSRenderCheapAvatarAlpha", false);
        static LLCachedControl<F32> distance(gSavedSettings, "FSRenderCheapAvatarAlphaDistance", 10.f);
        static LLCachedControl<S32> crowd(gSavedSettings, "FSRenderCheapAvatarAlphaCrowd", 0);
        static LLCachedControl<F32> coverage(gSavedSettings, "FSRenderCheapAvatarAlphaCoverage", 0.f);
        const F64 per_frame = 1.0 / (F64)sFrames;
        LL_INFOS("FSAvatarAlpha") << "Avatar alpha per frame over " << sFrames << " frames: "
                                  << llformat("%.1f draws (%.1f cheap, %.1f full), %.1f avatars (%.1f cheap, %.1f full); ",
                                              (F64)sDraws * per_frame, (F64)sCheapDraws * per_frame, (F64)(sDraws - sCheapDraws) * per_frame,
                                              (F64)sAvatars * per_frame, (F64)sCheapAvatars * per_frame, (F64)(sAvatars - sCheapAvatars) * per_frame)
                                  << (cheap ? llformat("FSRenderCheapAvatarAlpha on, beyond %.1f m", llclamp((F32)distance, 2.f, 64.f))
                                            : std::string("FSRenderCheapAvatarAlpha off"))
                                  << (cheap && crowd > 0 ? llformat(", or with %d or more other avatars drawn in full (%d now)", (S32)crowd, sCrowd) : std::string())
                                  << (cheap && coverage > 0.f ? llformat(", or covering under %.1f%% of the screen", (F32)coverage * 100.f) : std::string())
                                  << "; "
                                  << "GPU time: zone \"alpha avatars\" in the FSGPUTimings line"
                                  << (sUntimedGroups ? llformat(" (%.1f attachment groups per frame past the zone cap, untimed)", (F64)sUntimedGroups * per_frame) : std::string())
                                  << LL_ENDL;
        // FSRenderCheapAlphaDistance: the in-world alpha (everything the alpha pass draws besides avatars and
        // particles), and where the big surfaces are
        const F32 scene_dist = scene_distance();
        std::string groups;
        for (const SceneGroup& g : sSceneGroupsLast)
        {
            groups += llformat("\n    %s at <%.0f, %.0f, %.0f>: %.1f%% of the screen, %u draws, %uk triangles, %.0f m, %s",
                               g.mBridge ? llformat("object %s (moving or linked)", g.mId.asString().substr(0, 8).c_str()).c_str()
                                         : llformat("region cell with object %s", g.mId.asString().substr(0, 8).c_str()).c_str(),
                               g.mPosition.mV[0], g.mPosition.mV[1], g.mPosition.mV[2], g.mArea * 100.f, g.mDraws, (g.mTris + 500) / 1000,
                               g.mDistance, g.mCheap ? "lit cheaply" : "full lighting");
        }
        LL_INFOS("FSAvatarAlpha") << "In-world alpha (not avatars or particles) per frame over " << sFrames << " frames: "
                                  << llformat("%.1f draws (%.1f cheap); ", (F64)sSceneDraws * per_frame, (F64)sSceneCheapDraws * per_frame)
                                  << (scene_dist > 0.f ? llformat("FSRenderCheapAlphaDistance %.0f m", scene_dist) : std::string("FSRenderCheapAlphaDistance off"))
                                  << "; GPU time: the \"alpha\" zones less \"alpha avatars\" and \"alpha particles\". Biggest groups on screen in the last frame:"
                                  << (groups.empty() ? std::string(" none") : groups) << LL_ENDL;
        sFrames = 0;
        sDraws = sCheapDraws = sAvatars = sCheapAvatars = sUntimedGroups = 0;
        sSceneDraws = sSceneCheapDraws = 0;
    }
}

bool FSAvatarAlpha::beginPass()
{
    static LLCachedControl<bool> cheap(gSavedSettings, "FSRenderCheapAvatarAlpha", false);
    sFlaggedThisPass = false;
    // An impostor bake is lit flat by its programs (FOR_IMPOSTOR) anyway
    return (cheap || scene_distance() > 0.f) && !LLPipeline::sRenderingHUDs && !LLPipeline::sImpostorRender;
}

S32 FSAvatarAlpha::sceneGroup(LLSpatialGroup* group, const LLVector4a* ext, const std::vector<LLPointer<LLDrawInfo>>& draws, bool log_ok)
{
    if (!group)
    {
        return 0;
    }
    const F32 distance = scene_distance();
    const bool cheap = distance > 0.f && group->mDistance > distance;
    const S32 flags = cheap ? FSParticleRender::CHEAP : 0;
    sFlaggedThisPass = sFlaggedThisPass || flags != 0;
    if (!sStatsOn || !log_ok || !main_view())
    {
        return flags;
    }
    SceneGroup g;
    for (const LLPointer<LLDrawInfo>& draw : draws)
    {
        if (draw.notNull() && draw->mAvatar.isNull())
        {
            ++g.mDraws;
            g.mTris += draw->mCount / 3;
        }
    }
    if (!g.mDraws)
    {
        return flags;
    }
    sSceneDraws += g.mDraws;
    sSceneCheapDraws += cheap ? g.mDraws : 0;
    if (ext)
    {
        LLVector4a center;
        LLVector4a size;
        center.setAdd(ext[0], ext[1]);
        center.mul(0.5f);
        size.setSub(ext[1], ext[0]);
        size.mul(0.5f);
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 view_pixels = (F32)camera->getViewHeightInPixels() * (F32)camera->getViewHeightInPixels() * camera->getAspect();
        g.mArea = view_pixels > 0.f ? llmin(LLPipeline::calcPixelArea(center, size, *camera) / view_pixels, 1.f) : 0.f;
    }
    g.mDistance = group->mDistance;
    g.mCheap = cheap;
    // which object: a bridge's root, else the first drawable of the region cell
    LLSpatialBridge* bridge = group->getSpatialPartition() ? group->getSpatialPartition()->asBridge() : nullptr;
    const LLDrawable* drawable = bridge ? bridge->mDrawable : nullptr;
    g.mBridge = bridge != nullptr;
    if (!drawable)
    {
        for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
        {
            drawable = (const LLDrawable*)(*i)->getDrawable();
            if (drawable)
            {
                break;
            }
        }
    }
    const LLViewerObject* vobj = drawable ? drawable->getVObj() : nullptr;
    if (vobj)
    {
        g.mId = vobj->getID();
        g.mPosition = vobj->getPositionRegion();
    }
    sSceneGroups.push_back(g);
    return flags;
}

const LLVOAvatar* FSAvatarAlpha::groupAvatar(LLSpatialGroup* group)
{
    LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
    if (!part || part->mPartitionType != LLViewerRegion::PARTITION_AVATAR)
    {
        return nullptr;
    }
    LLSpatialBridge* bridge = part->asBridge();
    LLDrawable* root = bridge ? bridge->mDrawable : nullptr;     // the attachment's root
    LLViewerObject* vobj = root ? root->getVObj().get() : nullptr;
    if (!vobj || vobj->isDead())
    {
        return nullptr;
    }
    const LLVOAvatar* avatar = vobj->getAvatar();
    return (avatar && !avatar->isDead()) ? avatar : nullptr;
}

S32 FSAvatarAlpha::flags(const LLVOAvatar* avatar)
{
    if (!avatar)
    {
        return 0;
    }
    static LLCachedControl<S32> crowd_setting(gSavedSettings, "FSRenderCheapAvatarAlphaCrowd", 0);
    static LLCachedControl<F32> coverage_setting(gSavedSettings, "FSRenderCheapAvatarAlphaCoverage", 0.f);
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != sCacheFrame)
    {
        sCache.clear();
        sCacheFrame = frame;
        sLastAvatar = nullptr;
        sCrowd = crowd_setting > 0 ? count_crowd() : 0;
        sScreenPixels = gViewerWindow ? (F32)gViewerWindow->getWorldViewWidthRaw() * (F32)gViewerWindow->getWorldViewHeightRaw() : 0.f;
    }
    if (avatar != sLastAvatar)
    {
        auto it = sCache.find(avatar);
        if (it != sCache.end())
        {
            sLastFlags = it->second;
        }
        else
        {
            S32 flags = 0;
            static LLCachedControl<bool> cheap_avatars(gSavedSettings, "FSRenderCheapAvatarAlpha", false);
            if (cheap_avatars && !avatar->isSelf() && !avatar->isControlAvatar() && !avatar->isDead())
            {
                static LLCachedControl<F32> distance_setting(gSavedSettings, "FSRenderCheapAvatarAlphaDistance", 10.f);
                const F32 distance = llclamp((F32)distance_setting, 2.f, 64.f);
                const F32 dist_sq = dist_vec_squared(avatar->getPositionAgent(), gAgentCamera.getCameraPositionAgent());
                flags = dist_sq > distance * distance ? FSParticleRender::CHEAP : 0;
                // FSRenderCheapAvatarAlphaCrowd: with that many other avatars drawn in full, every one of them is cheap,
                // however near. FSRenderCheapAvatarAlphaCoverage: so is one whose box on screen (the pixel area the
                // impostor logic keeps, from the main camera) is under that share of the view.
                if (!flags && crowd_setting > 0 && sCrowd >= (S32)crowd_setting)
                {
                    flags = FSParticleRender::CHEAP;
                }
                if (!flags && coverage_setting > 0.f && sScreenPixels > 0.f &&
                    avatar->getPixelArea() < llclamp((F32)coverage_setting, 0.f, 1.f) * sScreenPixels)
                {
                    flags = FSParticleRender::CHEAP;
                }
            }
            sCache.emplace(avatar, flags);
            sLastFlags = flags;
        }
        sLastAvatar = avatar;
    }
    sFlaggedThisPass = sFlaggedThisPass || sLastFlags != 0;
    return sLastFlags;
}

bool FSAvatarAlpha::endPass()
{
    const bool flagged = sFlaggedThisPass;
    sFlaggedThisPass = false;
    return flagged;
}

bool FSAvatarAlpha::statsOn()
{
    return sStatsOn;
}

void FSAvatarAlpha::noteDraw(const LLVOAvatar* avatar, S32 flags)
{
    if (!sStatsOn || !avatar || avatar->isControlAvatar() || !main_view())
    {
        return; // animesh is never lit cheaply, and left out of the counts
    }
    ++sDraws;
    sCheapDraws += flags ? 1 : 0;
    sSeen.emplace(avatar, flags != 0);
}

// FSRenderAlphaRiggedNoDiscard
static bool fs_opaque_texture(const LLViewerTexture* tex)
{
    // 3 components only: 1 can be an alpha-only (invisiprim) texture, 2 and 4 carry alpha; a texture without a GL
    // texture yet is drawn with a placeholder
    return tex && tex->hasGLTexture() && tex->getComponents() == 3;
}

bool FSAvatarAlpha::noDiscardDraw(const LLDrawInfo& params)
{
    if (!params.mFSTintAboveMin || params.mGLTFMaterial.notNull())
    {
        return false;
    }
    if (params.mTextureList.size() > 1)
    {   // every texture of the list, picked by the vertex's texture index (an empty slot leaves an earlier draw's
        // texture on the unit: unknown)
        for (const LLPointer<LLViewerTexture>& tex : params.mTextureList)
        {
            if (!fs_opaque_texture(tex.get()))
            {
                return false;
            }
        }
        return true;
    }
    return fs_opaque_texture(params.mTexture.get());
}

F32 FSAvatarAlpha::tintOpaqueThreshold()
{
    static LLCachedControl<F32> threshold(gSavedSettings, "FSRenderAvatarTintOpaqueAlpha", 0.f);
    return threshold > 0.f ? llclamp((F32)threshold, 0.5f, 0.999f) : 0.f;
}

bool FSAvatarAlpha::tintOpaque(const LLTextureEntry* te, const LLViewerTexture* tex, const LLViewerObject* vobj)
{
    const F32 threshold = tintOpaqueThreshold();
    if (threshold <= 0.f || !te || !vobj || te->getGLTFRenderMaterial() || !vobj->isAttachment() || vobj->isHUDAttachment())
    {
        return false;
    }
    const F32 alpha = te->getColor().mV[3];
    // the components alone, not whether the GL texture exists yet: a face is rebuilt when its texture's component
    // count becomes known (LLViewerFetchedTexture, dirtyTexture), which may be before the GL texture is created
    return alpha >= threshold && alpha < 0.999f && tex && tex->getComponents() == 3;
}

bool FSAvatarAlpha::openZone()
{
    if (!LLFSGPUZone::sActive.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (sZonesThisFrame >= MAX_ZONES_PER_FRAME)
    {
        if (sStatsOn)
        {
            ++sUntimedGroups;
        }
        return false;
    }
    ++sZonesThisFrame;
    return true;
}

void FSAvatarAlpha::frame(bool enabled, F32 interval_seconds)
{
    static LLTimer timer;
    sZonesThisFrame = 0;
    if (enabled && sStatsOn)
    {
        end_frame();
        ++sFrames;
    }
    sSeen.clear();
    sSceneGroups.clear();
    if (enabled != sStatsOn)
    {
        sStatsOn = enabled;
        timer.reset();
        log_stats(); // what was gathered so far, or nothing
        sFrames = 0;
        sDraws = sCheapDraws = sAvatars = sCheapAvatars = sUntimedGroups = 0;
        sSceneDraws = sSceneCheapDraws = 0;
        sSceneGroupsLast.clear();
        return;
    }
    if (enabled && timer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
    {
        timer.reset();
        log_stats();
    }
}
