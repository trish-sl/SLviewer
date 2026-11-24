/**
 * @file fslighterwhilemoving.cpp
 * @brief FSRenderLighterWhileMoving: lighter frames while the camera walks, runs or turns
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

#include "fslighterwhilemoving.h"

#include "llagent.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llvovolume.h"

#include <unordered_map>
#include <unordered_set>

namespace
{
    FSMoving::ETier sTier = FSMoving::TIER_STILL;
    FSMoving::Stats sStats;

    // FSRenderMovingLODHold: objects whose detail change was held back, by id, so a removed object is
    // simply not found when its turn to catch up comes; with the time a decrease was first held back
    // (< 0: only increases were). A decrease held this long goes one level down while still moving.
    constexpr size_t MAX_HELD_LOD = 50000;
    constexpr F64 LOD_DECREASE_HOLD_SECONDS = 3.0;
    constexpr U32 CATCH_UP_PER_FRAME = 256;
    constexpr U32 CATCH_UP_PER_HEAVY_FRAME = 32;    // while the last frame's group rebuilds took over 4 ms
    std::unordered_map<LLUUID, F64> sHeldLOD;
    F32 sLastGroupMs = 0.f;

    // FSRenderMovingRebuildBudget: how long the budget stays on after the camera settles, so what the walk held
    // back is spread over a few frames instead of all landing in the first still one
    constexpr F32 REBUILD_SETTLE_SECONDS = 2.f;
    LLFrameTimer sSettleTimer;
    bool sSettling = false;

    // FSLimitFramerate's sleep at the end of the last frame, ms
    F32 sLimiterSleepMs = 0.f;

    // FSRenderMovingShadowCache: whether a drawable's last geometry update was a detail change only. Keyed by
    // pointer, only compared: an entry is taken when the drawable's group rebuilds, and a new drawable's first
    // update (a volume change) clears whatever an old one at the same address left.
    constexpr size_t MAX_LOD_ONLY = 16384;
    std::unordered_map<const LLDrawable*, bool> sLODOnly;

    bool master()
    {
        static LLCachedControl<bool> lighter(gSavedSettings, "FSRenderLighterWhileMoving", false);
        return lighter;
    }

    void logStats()
    {
        const FSMoving::Stats& s = sStats;
        const F64 budget_avg = s.mBudgetFrames ? s.mBudgetMsSum / s.mBudgetFrames : 0.0;
        const F64 group_avg = s.mBudgetFrames ? s.mGroupMsSum / s.mBudgetFrames : 0.0;
        LL_INFOS("FSMotion") << llformat("Camera still %.1f s, walking %.1f s, fast %.1f s", s.mTierSeconds[FSMoving::TIER_STILL],
                                         s.mTierSeconds[FSMoving::TIER_WALKING], s.mTierSeconds[FSMoving::TIER_FAST])
                             << llformat("; rebuild budget: %u of %u moving frames cut short, budget %.1f ms, group rebuilds %.1f ms "
                                         "a frame (peak %.1f)", s.mBudgetThrottled, s.mBudgetFrames, budget_avg, group_avg, s.mGroupMsPeak)
                             << llformat("; detail changes held: %u decreases, %u increases, %u caught up when still",
                                         s.mLODHeldDecrease, s.mLODHeldIncrease, s.mLODCaughtUp)
                             << llformat("; texture bias held %.1f s", s.mBiasHoldSeconds)
                             << llformat("; far shadow cascades: %u held, %u on their interval, forced: %u slice left the map, "
                                         "%u depth range, %u map had no receivers, %u sun, view, snapshot or interval 1, %u camera moved",
                                         s.mCascadeHeld, s.mCascadeDue, s.mCascadeForced[FSMoving::FORCE_SLICE],
                                         s.mCascadeForced[FSMoving::FORCE_DEPTH], s.mCascadeForced[FSMoving::FORCE_NO_MAP],
                                         s.mCascadeForced[FSMoving::FORCE_VIEW], s.mCascadeForced[FSMoving::FORCE_MOVED])
                             << llformat("; static shadow cache: %u small changes ignored, %u detail changes deferred, %u applied, "
                                         "%u reused over deferred changes, %u stored while moving", s.mCacheSmallIgnored,
                                         s.mCacheLODDeferred, s.mCacheLODApplied, s.mCacheDeferredReuse, s.mCacheStoredMoving)
                             << llformat("; probe refreshes put off %u frames", s.mProbeSkipped)
                             << llformat("; occlusion reads: %u collected, %u not in yet, %u untracked (asked again), %u waited",
                                         s.mOcclusionCached, s.mOcclusionPending, s.mOcclusionUntracked, s.mOcclusionWaited)
                             << LL_ENDL;
    }
}

namespace FSMoving
{
    void updateTier(F32 speed, F32 turn_rate, bool fast, F32 dt)
    {
        // Smoothed over about a quarter second: a single frame's jump (a teleport, a region crossing) is the
        // fast test's business, and frame time spikes don't make a walk look like a stop
        static F32 s_speed = 0.f;
        static F32 s_turn = 0.f;
        static bool s_walking = false;
        static LLFrameTimer s_hold;
        dt = llclamp(dt, 0.001f, 0.25f);
        const F32 k = llmin(dt / 0.25f, 1.f);
        // A teleport or region crossing jumps the camera in one frame: no walk, and the smoothing would
        // carry it into the next second. That frame keeps the speed as it was.
        if (speed > 40.f || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        {
            speed = s_speed;
        }
        s_speed += (llclamp(speed, 0.f, 30.f) - s_speed) * k;
        s_turn += (llclamp(turn_rate, 0.f, 3.f) - s_turn) * k;
        // Walking is 3.2 m/s, running 6.5. Entering takes a clear step or turn (8.6 degrees a second); staying
        // takes less, and the tier holds for half a second after, so stopping to look around doesn't flicker.
        const bool over = s_walking ? (s_speed > 0.3f || s_turn > 0.08f) : (s_speed > 0.6f || s_turn > 0.15f);
        if (over)
        {
            s_hold.resetWithExpiry(0.5f);
        }
        s_walking = over || !s_hold.hasExpired();

        const ETier last_tier = sTier;
        sTier = !master() ? TIER_STILL : fast ? TIER_FAST : s_walking ? TIER_WALKING : TIER_STILL;
        if (sTier != TIER_STILL)
        {
            sSettling = false;
        }
        else if (last_tier != TIER_STILL && master())
        {
            sSettling = true;
            sSettleTimer.reset();
        }
        sStats.mTierSeconds[sTier] += dt;

        if (sTier == TIER_STILL && !sLODOnly.empty())
        {
            sLODOnly.clear();
        }

        static LLFrameTimer s_log_timer;
        if (s_log_timer.getElapsedTimeF32() >= 10.f)
        {
            s_log_timer.reset();
            if (statsEnabled())
            {
                logStats();
            }
            sStats = Stats();
        }
    }

    bool enabled()
    {
        return master();
    }

    ETier tier()
    {
        return sTier;
    }

    bool moving()
    {
        return sTier != TIER_STILL;
    }

    bool rebuildBudget()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingRebuildBudget", false);
        return option && moving();
    }

    bool rebuildSettling()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingRebuildBudget", false);
        if (sSettling && (!option || !master() || sSettleTimer.getElapsedTimeF32() > REBUILD_SETTLE_SECONDS))
        {
            sSettling = false;
        }
        return sSettling;
    }

    bool lodHold()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingLODHold", false);
        return option && moving();
    }

    bool shadowReduce()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingShadowReduce", false);
        static LLCachedControl<bool> reduce_while_moving(gSavedSettings, "FSRenderShadowReduceWhileMoving", false);
        return option && reduce_while_moving && sTier == TIER_WALKING;
    }

    bool textureBiasHold()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingTextureBiasHold", false);
        return option && moving();
    }

    bool shadowHoldEnabled()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingShadowHold", false);
        return option && master();
    }

    bool shadowCacheEnabled()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingShadowCache", false);
        return option && master();
    }

    bool probeHold()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMovingProbeHold", false);
        return option && moving();
    }

    bool asyncOcclusion()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderAsyncOcclusionReadback", false);
        return option;
    }

    bool statsEnabled()
    {
        static LLCachedControl<bool> option(gSavedSettings, "FSRenderMotionStats", false);
        return option;
    }

    F32 frameRebuildBudgetMs(F32 max_budget_ms, F32 last_frame_ms, F32 last_group_ms)
    {
        // The frame rate aimed for: the limiter's when it is on, else 60
        static LLCachedControl<bool> limit_framerate(gSavedSettings, "FSLimitFramerate", true);
        static LLCachedControl<U32> max_fps(gSavedSettings, "FramePerSecondLimit", 60);
        const F32 fps = (limit_framerate && max_fps > 0) ? llclamp((F32)max_fps, 15.f, 240.f) : 60.f;
        const F32 target_ms = 1000.f / fps;
        // What the frame needs besides the group rebuilds. Over the target already, the rebuilds get the floor:
        // they still go on (what is in view first), only spread over more frames. The limiter's sleep is no work:
        // counted in, a capped frame rate left the rebuilds only what they took last frame.
        const F32 sleep_ms = (limit_framerate && max_fps > 0) ? sLimiterSleepMs : 0.f;
        const F32 other_ms = llmax(last_frame_ms - last_group_ms - sleep_ms, 0.f);
        const F32 floor_ms = llmin(1.f, max_budget_ms);
        return llclamp(target_ms - other_ms, floor_ms, llmax(max_budget_ms, floor_ms));
    }

    void noteLimiterSleep(S32 ms)
    {
        sLimiterSleepMs = (F32)llmax(ms, 0);
    }

    void noteGroupRebuildMs(F32 ms)
    {
        sLastGroupMs = ms;
    }

    bool noteLODHeld(const LLUUID& id, bool decrease)
    {
        if (sHeldLOD.size() >= MAX_HELD_LOD && !sHeldLOD.count(id))
        {
            return false;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        auto it = sHeldLOD.find(id);
        if (it == sHeldLOD.end())
        {
            sHeldLOD.emplace(id, decrease ? now : -1.0);
        }
        else if (decrease && it->second < 0.0)
        {
            it->second = now;
        }
        ++(decrease ? sStats.mLODHeldDecrease : sStats.mLODHeldIncrease);
        return true;
    }

    bool lodDecreaseDue(const LLUUID& id)
    {
        auto it = sHeldLOD.find(id);
        if (it == sHeldLOD.end() || it->second < 0.0)
        {
            return false;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - it->second < LOD_DECREASE_HOLD_SECONDS)
        {
            return false;
        }
        it->second = now;
        return true;
    }

    void catchUpLOD()
    {
        if (sHeldLOD.empty() || lodHold())
        {
            return;
        }
        LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
        // Each caught up object may rebuild its whole group: fewer a frame while those already run heavy
        const U32 per_frame = sLastGroupMs > 4.f ? CATCH_UP_PER_HEAVY_FRAME : CATCH_UP_PER_FRAME;
        U32 count = 0;
        for (auto it = sHeldLOD.begin(); it != sHeldLOD.end() && count < per_frame; ++count)
        {
            LLViewerObject* object = gObjectList.findObject(it->first);
            it = sHeldLOD.erase(it);
            LLVOVolume* volume = object && !object->isDead() ? dynamic_cast<LLVOVolume*>(object) : nullptr;
            if (volume && volume->mDrawable.notNull() && !volume->mDrawable->isDead() && volume->getVolume())
            {
                // the detail it would have had now, worked out from the distance it was last seen at
                if (volume->updateLOD())
                {
                    ++sStats.mLODCaughtUp;
                }
            }
        }
    }

    void noteVolumeUpdate(const LLDrawable* drawable, bool lod_only)
    {
        if (!drawable || !moving() || !shadowCacheEnabled())
        {
            return;
        }
        if (sLODOnly.size() >= MAX_LOD_ONLY)
        {
            sLODOnly.clear(); // forgotten entries only make changes count as more than detail changes
        }
        // Several updates before the group rebuilds (the rebuild budget can hold one back): detail only if all were
        auto it = sLODOnly.find(drawable);
        if (it != sLODOnly.end())
        {
            it->second = it->second && lod_only;
        }
        else
        {
            sLODOnly.emplace(drawable, lod_only);
        }
    }

    bool takeLODOnly(const LLDrawable* drawable)
    {
        if (sLODOnly.empty())
        {
            return false;
        }
        auto it = sLODOnly.find(drawable);
        if (it == sLODOnly.end())
        {
            return false;
        }
        const bool lod_only = it->second;
        sLODOnly.erase(it);
        return lod_only;
    }

    Stats& stats()
    {
        return sStats;
    }
}
