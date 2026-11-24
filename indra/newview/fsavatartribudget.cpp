/**
 * @file fsavatartribudget.cpp
 * @brief A triangle budget per avatar (FSRenderAvatarTriangleBudget): the largest rigged meshes step down a detail level
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

#include "fsavatartribudget.h"

#include "lldrawable.h"
#include "llmeshrepository.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"
#include "llvovolume.h"

#include <algorithm>
#include <string>
#include <vector>

namespace FSAvatarTriangleBudgetImpl
{
    struct Mesh
    {
        LLVOVolume* mVolume = nullptr;
        S32 mLOD = 0;           // the level the camera's distance gives it
        S32 mBias = 0;          // levels taken off
        F32 mEst[4] = {};       // triangles at each level, 0 lowest
        bool mFixed = false;    // no header yet: its current triangles, never stepped
    };

    std::vector<Mesh> sMeshes;      // one avatar's, reused
    bool sWasOn = false;

    // summed over the log interval
    U32 sFrames = 0;
    U32 sAvatarsOver = 0;           // avatar frames over budget before stepping
    U32 sAvatarsStillOver = 0;      // and still over after (every mesh at its lowest)
    U64 sTrisBefore = 0;
    U64 sTrisAfter = 0;
    std::string sWorst;             // the avatar stepped down most in the last frame
    U32 sWorstSteps = 0;

    // The rigged meshes of the object and its children
    void collect(LLViewerObject* obj)
    {
        if (!obj || obj->isDead() || obj->isHUDAttachment())
        {
            return;
        }
        LLVOVolume* vol = dynamic_cast<LLVOVolume*>(obj);
        if (vol && vol->mDrawable.notNull() && vol->mDrawable->isState(LLDrawable::RIGGED))
        {
            Mesh m;
            m.mVolume = vol;
            m.mLOD = vol->mFSUnbiasedLOD >= 0 ? vol->mFSUnbiasedLOD : llclamp(vol->getLOD(), 0, 3);
            LLMeshCostData costs;
            if (vol->getCostData(costs))
            {
                for (S32 i = 0; i < 4; ++i)
                {
                    m.mEst[i] = llmax(costs.getEstTrisByLOD(i), 0.f);
                }
            }
            else
            {
                m.mFixed = true;
                const F32 tris = (F32)vol->getTriangleCount();
                for (S32 i = 0; i < 4; ++i)
                {
                    m.mEst[i] = tris;
                }
            }
            sMeshes.push_back(m);
        }
        for (const auto& child : obj->getChildren())
        {
            collect(child);
        }
    }

    // Clears every bias of the avatar's rigged meshes (the setting went off)
    void clear(LLViewerObject* obj)
    {
        if (!obj || obj->isDead())
        {
            return;
        }
        LLVOVolume* vol = dynamic_cast<LLVOVolume*>(obj);
        if (vol)
        {
            vol->mFSLODBias = 0;
        }
        for (const auto& child : obj->getChildren())
        {
            clear(child);
        }
    }

    void budget_avatar(LLVOAvatar* avatar, F32 budget)
    {
        sMeshes.clear();
        for (const auto& child : avatar->getChildren())
        {
            collect(child);
        }
        F32 sum = 0.f;
        for (const Mesh& m : sMeshes)
        {
            sum += m.mEst[m.mLOD];
        }
        const F32 before = sum;
        U32 steps = 0;
        while (sum > budget)
        {
            // the mesh with the most triangles at its current level that can still go down
            Mesh* pick = nullptr;
            for (Mesh& m : sMeshes)
            {
                if (!m.mFixed && m.mLOD - m.mBias > 0 && (!pick || m.mEst[m.mLOD - m.mBias] > pick->mEst[pick->mLOD - pick->mBias]))
                {
                    pick = &m;
                }
            }
            if (!pick)
            {
                break;
            }
            const S32 level = pick->mLOD - pick->mBias;
            sum -= pick->mEst[level] - pick->mEst[level - 1];
            ++pick->mBias;
            ++steps;
        }
        for (const Mesh& m : sMeshes)
        {
            m.mVolume->mFSLODBias = m.mBias;    // calcLOD takes it off the level it computes
        }
        if (before > budget)
        {
            ++sAvatarsOver;
            sAvatarsStillOver += sum > budget ? 1 : 0;
            sTrisBefore += (U64)before;
            sTrisAfter += (U64)sum;
            if (steps > sWorstSteps)
            {
                sWorstSteps = steps;
                sWorst = llformat("%s: %.0fk triangles at the camera's detail, %.0fk after stepping %u meshes down a level",
                                  avatar->getFullname().c_str(), before / 1000.f, sum / 1000.f, steps);
            }
        }
    }
}

using namespace FSAvatarTriangleBudgetImpl;

void FSAvatarTriangleBudget::frame(bool log_enabled, F32 interval_seconds)
{
    static LLCachedControl<U32> budget_setting(gSavedSettings, "FSRenderAvatarTriangleBudget", 0);
    static LLTimer timer;
    const U32 budget = budget_setting;
    if (!budget)
    {
        if (sWasOn)
        {
            sWasOn = false;
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
                if (avatar && !avatar->isDead())
                {
                    for (const auto& child : avatar->getChildren())
                    {
                        clear(child);
                    }
                }
            }
        }
        return;
    }
    if (!sWasOn)
    {
        sWasOn = true;
        timer.reset();
    }

    ++sFrames;
    sWorstSteps = 0;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead() || avatar->isSelf() || avatar->isControlAvatar() || avatar->isImpostor() ||
            avatar->isVisuallyMuted() || !avatar->isVisible())
        {
            continue;
        }
        budget_avatar(avatar, (F32)budget);
    }

    if (log_enabled && timer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
    {
        timer.reset();
        if (sFrames)
        {
            LL_INFOS("FSAvatarCost") << "Avatar triangle budget " << budget << ": per frame over " << sFrames << " frames, "
                                     << llformat("%.1f avatars over budget (%.1f still over with every mesh at its lowest level), %.0fk triangles brought down to %.0fk",
                                                 (F64)sAvatarsOver / sFrames, (F64)sAvatarsStillOver / sFrames,
                                                 (F64)sTrisBefore / sFrames / 1000.0, (F64)sTrisAfter / sFrames / 1000.0)
                                     << (sWorst.empty() ? std::string() : "; most stepped in the last frame: " + sWorst) << LL_ENDL;
        }
        sFrames = 0;
        sAvatarsOver = sAvatarsStillOver = 0;
        sTrisBefore = sTrisAfter = 0;
        sWorst.clear();
    }
}
