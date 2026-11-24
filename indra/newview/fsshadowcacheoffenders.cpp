/**
 * @file fsshadowcacheoffenders.cpp
 * @brief FSPerfShadowStaticCache: which objects make the far shadow cache re-render, and keeping
 *        objects that keep changing out of it
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

#include "fsshadowcacheoffenders.h"

#include "llagent.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "workqueue.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using namespace FSShadowCacheOffenders;

    const char* const KIND_NAMES[KIND_COUNT] = { "changed", "added", "removed", "rebuilt", "recoloured", "regrouped" };

    // Why an object was demoted
    enum EWhy : U8 { WHY_REPEATED = 0, WHY_OMEGA, WHY_FLEXIBLE, WHY_TEXTURE_ANIM };
    const char* const WHY_NAMES[] = { "keeps changing", "target omega", "flexible", "texture animation" };

    constexpr F32 REPEAT_WINDOW = 10.f;         // seconds: this many drops in this window demote
    constexpr U32 REPEAT_COUNT = 3;
    constexpr F32 REPEAT_SPACING = 1.f;         // drops closer than this count once (detail levels arriving)
    constexpr F32 CAMERA_SETTLE = 2.f;          // seconds after the camera moved before rebuilds count
    constexpr F32 ARRIVAL_SETTLE = 60.f;        // seconds after arriving in a region before anything counts (loading)
    constexpr F32 STILL_FIRST = 60.f;           // seconds still before a demoted object is made static again
    constexpr F32 STILL_MAX = 960.f;
    constexpr F32 PROMOTE_POLL = 2.f;
    constexpr size_t MAX_DEMOTED = 128;
    constexpr F32 LOG_PERIOD = 10.f;

    struct Culprit
    {
        LLUUID  mRoot;          // linkset root, null for a group as a whole (or terrain)
        U8      mKind = KIND_UNKNOWN;
        bool    mMesh = false;
    };

    // What a period of FSPerfShadowBatchStats saw of one root
    struct Offender
    {
        U32     mCount = 0;
        U8      mKind = KIND_UNKNOWN;
        bool    mMesh = false;
        F32     mSize = 0.f;    // the group's largest size, FLT_MAX for terrain
        F32     mDist = 0.f;
    };

    // Recent drops of one root, for demotion
    struct Recent
    {
        F32     mTimes[REPEAT_COUNT] = {};
        U32     mCount = 0;     // how many of mTimes are in use (newest last)
    };

    struct Demoted
    {
        U64     mSignature = 0;
        F32     mStillSince = 0.f;
        F32     mCooldown = STILL_FIRST;
    };

    Culprit sCulprit;
    std::vector<Culprit> sEntries;                      // one per extents pair of LLPipeline::mFSShadowDirty
    std::unordered_map<LLUUID, Offender> sOffenders;    // this log period
    U32 sGroupDrops = 0;                                // drops with no object to blame, this period
    std::unordered_map<LLUUID, U8> sFrameHits;          // roots that dropped a cache this update -> EKind
    std::unordered_map<LLUUID, Recent> sRecent;
    std::unordered_map<LLUUID, Demoted> sDemoted;
    std::unordered_map<LLUUID, F32> sBackoff;           // the cooldown for the next demotion of a root
    std::unordered_set<LLUUID> sPending;                // demotions posted, not applied yet
    U32 sGeneration = 0;                                // bumped by every reset, so a posted demotion of an older one is dropped

    U32 sBridgesSkipped = 0;
    U32 sSmallIgnored = 0;
    std::string sEvents;                                // demotions and promotions this period

    LLFrameTimer sLogTimer;
    LLFrameTimer sPollTimer;
    const LLViewerRegion* sRegion = nullptr;
    LLVector3 sCamOrigin;
    LLVector3 sCamAt;
    F32 sCamMovedAt = -1000.f;
    F32 sArrivedAt = 0.f;

    bool statsOn()
    {
        static LLCachedControl<bool> batch_stats(gSavedSettings, "FSPerfShadowBatchStats", false);
        return batch_stats;
    }

    bool demoteOn()
    {
        static LLCachedControl<bool> demote(gSavedSettings, "FSPerfShadowStaticCacheDemote", true);
        return demote;
    }

    F32 now()
    {
        return (F32)LLFrameTimer::getElapsedSeconds();
    }

    // A known animation of any prim of the linkset, as an EWhy, or -1
    S8 animationOf(const LLViewerObject* root)
    {
        auto one = [](const LLViewerObject* obj) -> S8
        {
            if (!obj)
            {
                return -1;
            }
            if (obj->getAngularVelocity().lengthSquared() > 0.f)
            {
                return WHY_OMEGA;
            }
            if (obj->isFlexible())
            {
                return WHY_FLEXIBLE;
            }
            const LLDrawable* drawable = obj->mDrawable;
            const LLVOVolume* volume = drawable ? drawable->getVOVolume() : nullptr;
            if (volume && volume->mTextureAnimp)
            {
                return WHY_TEXTURE_ANIM;
            }
            return -1;
        };
        S8 why = one(root);
        if (why < 0 && root)
        {
            for (const LLViewerObject* child : root->getChildren())
            {
                why = one(child);
                if (why >= 0)
                {
                    break;
                }
            }
        }
        return why;
    }

    // Whether the linkset may be made static again: what LLDrawable::makeStatic and the viewer itself would
    // not undo or refuse (as fs_avatar_only_can_restore in pipeline.cpp)
    bool canPromote(const LLViewerObject* root)
    {
        if (!root || root->isDead() || root->isAttachment() || root->isFlexible() || root->isAnimatedObject() ||
            root->isSelected() || root->getVelocity().lengthSquared() > 0.f || root->getAngularVelocity().lengthSquared() > 0.f)
        {
            return false;
        }
        for (const LLViewerObject* child : root->getChildren())
        {
            if (child && (child->isAvatar() || child->isFlexible() || child->isSelected() ||
                          child->getVelocity().lengthSquared() > 0.f || child->getAngularVelocity().lengthSquared() > 0.f))
            {
                return false;
            }
        }
        return animationOf(root) < 0;
    }

    // Changes whenever the linkset moves, turns, scales or has a group of it rebuilt
    U64 signatureOf(const LLViewerObject* root)
    {
        U64 h = 1469598103934665603ULL;
        auto add = [&h](F32 v)
        {
            U32 bits = 0;
            memcpy(&bits, &v, sizeof(bits));
            h = (h ^ bits) * 1099511628211ULL;
        };
        auto one = [&add](const LLViewerObject* obj)
        {
            const LLVector3& pos = obj->getPosition();
            const LLQuaternion& rot = obj->getRotation();
            const LLVector3& scale = obj->getScale();
            add(pos.mV[0]); add(pos.mV[1]); add(pos.mV[2]);
            add(rot.mQ[0]); add(rot.mQ[1]); add(rot.mQ[2]); add(rot.mQ[3]);
            add(scale.mV[0]); add(scale.mV[1]); add(scale.mV[2]);
            const LLDrawable* drawable = obj->mDrawable;
            const LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
            add(group ? group->mLastUpdateTime : 0.f);
        };
        one(root);
        for (const LLViewerObject* child : root->getChildren())
        {
            if (child && !child->isAvatar())
            {
                one(child);
            }
        }
        return h;
    }

    std::string describe(const LLUUID& id)
    {
        return id.notNull() ? id.asString() : std::string("(group)");
    }

    void addEvent(const std::string& text)
    {
        if (sEvents.size() < 2048)
        {
            sEvents += sEvents.empty() ? text : ", " + text;
        }
    }

    // Runs from the main loop's work queue, outside rendering: changing the octree in the middle of the shadow
    // pass that noticed the object would pull groups out from under it
    void applyDemotion(const LLUUID& id, U8 why, U32 generation)
    {
        sPending.erase(id);
        if (generation != sGeneration || sDemoted.size() >= MAX_DEMOTED || sDemoted.count(id))
        {
            return;
        }
        LLViewerObject* obj = gObjectList.findObject(id);
        LLDrawable* drawable = obj ? obj->mDrawable.get() : nullptr;
        if (!obj || obj->isDead() || obj->isAvatar() || obj->isAttachment() || obj->getPCode() != LL_PCODE_VOLUME ||
            !drawable || drawable->isDead() || !drawable->isRoot() || drawable->isActive() || !drawable->getVOVolume())
        {
            return;
        }
        drawable->makeActive();
        Demoted rec;
        rec.mSignature = signatureOf(obj);
        rec.mStillSince = now();
        auto backoff = sBackoff.find(id);
        rec.mCooldown = backoff != sBackoff.end() ? backoff->second : STILL_FIRST;
        sDemoted[id] = rec;
        addEvent(llformat("demoted %s (%s, %u prims)", id.asString().c_str(), WHY_NAMES[llmin((U32)why, 3U)],
                          (U32)obj->getChildren().size() + 1));
    }

    void applyPromotion(const LLUUID& id, U32 generation)
    {
        if (generation != sGeneration)
        {
            return;
        }
        LLViewerObject* obj = gObjectList.findObject(id);
        LLDrawable* drawable = obj ? obj->mDrawable.get() : nullptr;
        if (!obj || obj->isDead() || !drawable || drawable->isDead() || !drawable->isRoot() || !drawable->isActive() ||
            !canPromote(obj))
        {
            return;
        }
        drawable->makeStatic(false);
        addEvent(llformat("made static again %s", id.asString().c_str()));
    }

    void postDemotion(const LLUUID& id, U8 why)
    {
        if (sDemoted.size() + sPending.size() >= MAX_DEMOTED || sDemoted.count(id) || !sPending.insert(id).second)
        {
            return;
        }
        LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("mainloop");
        const U32 generation = sGeneration;
        if (!queue || !queue->post([id, why, generation]() { applyDemotion(id, why, generation); }))
        {
            sPending.erase(id);
        }
    }

    void forgetAll()
    {
        ++sGeneration;
        sArrivedAt = now();
        sOffenders.clear();
        sFrameHits.clear();
        sRecent.clear();
        sDemoted.clear();
        sBackoff.clear();
        sPending.clear();
        sGroupDrops = 0;
    }

    void logPeriod()
    {
        std::vector<std::pair<LLUUID, Offender>> top(sOffenders.begin(), sOffenders.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.mCount > b.second.mCount; });
        std::string who;
        for (size_t i = 0; i < top.size() && i < 5; ++i)
        {
            const Offender& o = top[i].second;
            const std::string size = o.mSize >= FLT_MAX ? std::string("terrain") : llformat("%.1f m", o.mSize);
            // five strings, then the distance and the count (a sixth %s read the distance as a pointer and crashed)
            who += llformat("%s%s %s (%s, group %s, %.0f m away) x%u", i ? ", " : "", describe(top[i].first).c_str(),
                            o.mMesh ? "mesh" : "prim", KIND_NAMES[llmin((U32)o.mKind, (U32)KIND_COUNT - 1)], size.c_str(),
                            (double)o.mDist, o.mCount);
        }
        if (who.empty() && !sBridgesSkipped && !sSmallIgnored && sEvents.empty() && sDemoted.empty())
        {
            return;
        }
        LL_INFOS("FSShadowBatches") << "Static shadow cache dropped by: " << (who.empty() ? std::string("nothing") : who)
                                    << llformat(" (%u objects in all, %u drops by groups regrouped or terrain)", (U32)sOffenders.size(), sGroupDrops)
                                    << llformat("; ignored: %u moving objects passing through static groups, %u changes too small to cast",
                                                sBridgesSkipped, sSmallIgnored)
                                    << llformat("; %u objects kept out of the cache", (U32)sDemoted.size())
                                    << (sEvents.empty() ? std::string() : "; " + sEvents) << LL_ENDL;
    }
}

namespace FSShadowCacheOffenders
{
    bool filterEnabled()
    {
        static LLCachedControl<bool> filter(gSavedSettings, "FSPerfShadowStaticCacheFilter", true);
        return filter;
    }

    bool tracking()
    {
        return statsOn() || demoteOn();
    }

    void setCulprit(const LLDrawable* drawable, EKind kind)
    {
        sCulprit = Culprit();
        sCulprit.mKind = kind;
        if (!drawable || !tracking())
        {
            return;
        }
        const LLViewerObject* obj = drawable->getVObj();
        const LLViewerObject* root = obj ? obj->getRootEdit() : nullptr;
        if (!root || root->isDead() || root->isAvatar())
        {
            return;
        }
        sCulprit.mRoot = root->getID();
        sCulprit.mMesh = obj->isMesh();
    }

    void clearCulprit()
    {
        sCulprit = Culprit();
    }

    void noteBridgeSkipped()
    {
        ++sBridgesSkipped;
    }

    void pushEntry(size_t pair_index)
    {
        if (sEntries.size() != pair_index)
        {
            sEntries.resize(pair_index);
        }
        sEntries.push_back(sCulprit);
    }

    void clearEntries()
    {
        sEntries.clear();
    }

    void noteInvalidation(size_t pair_index, const LLVector4a& lo, const LLVector4a& hi, F32 caster_size)
    {
        const Culprit culprit = pair_index < sEntries.size() ? sEntries[pair_index] : Culprit();
        if (culprit.mRoot.isNull())
        {
            ++sGroupDrops;
        }
        if (statsOn())
        {
            Offender& o = sOffenders[culprit.mRoot];
            ++o.mCount;
            o.mKind = culprit.mKind;
            o.mMesh = culprit.mMesh;
            o.mSize = caster_size;
            LLVector4a center;
            center.setAdd(lo, hi);
            center.mul(0.5f);
            const LLVector3 c(center.getF32ptr());
            o.mDist = (c - LLViewerCamera::getInstance()->getOrigin()).length();
        }
        if (culprit.mRoot.notNull())
        {
            sFrameHits[culprit.mRoot] = culprit.mKind;
        }
    }

    void noteSmallIgnored()
    {
        ++sSmallIgnored;
    }

    void update()
    {
        const F32 t = now();

        // A new region or a teleport: what is known is about objects left behind
        const LLViewerRegion* region = gAgent.getRegion();
        if (region != sRegion || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        {
            sRegion = region;
            forgetAll();
        }

        // Rebuilds right after the camera moved are mostly detail changes, which don't say an object animates
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        if ((camera->getOrigin() - sCamOrigin).lengthSquared() > 0.01f || camera->getAtAxis() * sCamAt < 0.9995f)
        {
            sCamOrigin = camera->getOrigin();
            sCamAt = camera->getAtAxis();
            sCamMovedAt = t;
        }
        const bool camera_settled = t - sCamMovedAt > CAMERA_SETTLE;

        const bool demote = demoteOn();
        // While a region loads, objects are rebuilt again and again as their detail levels, meshes and textures
        // arrive: that doesn't say they animate
        if (demote && t - sArrivedAt > ARRIVAL_SETTLE)
        {
            for (const auto& [id, kind] : sFrameHits)
            {
                if (sDemoted.count(id) || sPending.count(id))
                {
                    continue;
                }
                // Known to move on its own (target omega, flexible): no need to wait for it to drop the cache
                // again. Not for being added (everything is, as it loads), and not for texture animation alone,
                // which doesn't rebuild an object: such an object goes by the repeats like any other.
                const LLViewerObject* obj = (kind == KIND_REBUILT || kind == KIND_RECOLOURED) ? gObjectList.findObject(id) : nullptr;
                const S8 animated = obj && !obj->isDead() ? animationOf(obj) : (S8)-1;
                if (animated == WHY_OMEGA || animated == WHY_FLEXIBLE)
                {
                    postDemotion(id, (U8)animated);
                    continue;
                }
                if (!camera_settled && kind == KIND_REBUILT)
                {
                    continue;
                }
                Recent& recent = sRecent[id];
                if (recent.mCount && t - recent.mTimes[recent.mCount - 1] < REPEAT_SPACING)
                {
                    continue;
                }
                if (recent.mCount == REPEAT_COUNT)
                {
                    std::move(recent.mTimes + 1, recent.mTimes + REPEAT_COUNT, recent.mTimes);
                    --recent.mCount;
                }
                recent.mTimes[recent.mCount++] = t;
                if (recent.mCount == REPEAT_COUNT && t - recent.mTimes[0] <= REPEAT_WINDOW)
                {
                    sRecent.erase(id);
                    postDemotion(id, WHY_REPEATED);
                }
            }
        }
        sFrameHits.clear();

        // Demoted objects that have been still long enough go back into the static octree
        if (sPollTimer.getElapsedTimeF32() >= PROMOTE_POLL)
        {
            sPollTimer.reset();
            for (auto it = sDemoted.begin(); it != sDemoted.end();)
            {
                LLViewerObject* obj = gObjectList.findObject(it->first);
                LLDrawable* drawable = obj ? obj->mDrawable.get() : nullptr;
                if (!obj || obj->isDead() || !drawable || drawable->isDead() || drawable->isStatic())
                {
                    it = sDemoted.erase(it);    // gone, or made static by someone else
                    continue;
                }
                Demoted& rec = it->second;
                const U64 signature = signatureOf(obj);
                if (signature != rec.mSignature || animationOf(obj) >= 0)
                {
                    rec.mSignature = signature;
                    rec.mStillSince = t;
                }
                if (!demote || (t - rec.mStillSince >= rec.mCooldown && canPromote(obj)))
                {
                    const LLUUID id = it->first;
                    sBackoff[id] = llmin(rec.mCooldown * 2.f, STILL_MAX);
                    LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("mainloop");
                    const U32 generation = sGeneration;
                    if (queue)
                    {
                        queue->post([id, generation]() { applyPromotion(id, generation); });
                    }
                    it = sDemoted.erase(it);
                    continue;
                }
                ++it;
            }
            // drops older than the window can't add up to a demotion any more
            for (auto it = sRecent.begin(); it != sRecent.end();)
            {
                if (!it->second.mCount || t - it->second.mTimes[it->second.mCount - 1] > REPEAT_WINDOW)
                {
                    it = sRecent.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            if (sBackoff.size() > 4096)
            {
                sBackoff.clear();
            }
        }

        if (sLogTimer.getElapsedTimeF32() >= LOG_PERIOD)
        {
            sLogTimer.reset();
            if (statsOn())
            {
                logPeriod();
            }
            sOffenders.clear();
            sGroupDrops = 0;
            sBridgesSkipped = 0;
            sSmallIgnored = 0;
            sEvents.clear();
        }
    }

    void reset()
    {
        if (!sDemoted.empty() || !sOffenders.empty() || !sRecent.empty() || !sPending.empty())
        {
            forgetAll();
        }
        sEntries.clear();
        sCulprit = Culprit();
    }
}
