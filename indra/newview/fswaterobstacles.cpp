/**
 * @file fswaterobstacles.cpp
 * @brief Interactive water: the obstacle mask that ripples reflect off and flow around (FSRenderWaterRippleObstacles)
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

#include "fswaterobstacles.h"

#include "llagent.h"
#include "lldrawable.h"
#include "llface.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llspatialpartition.h"
#include "lltextureentry.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llvieweroctree.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "lltimer.h"

#include <cmath>
#include <vector>

namespace
{
    // A prim counts when its box reaches within this of the water surface (metres)
    constexpr F32 BAND = 0.25f;
    // Solid is tested on a plane this far above the water, so a hull or a float resting on the
    // surface counts even when its underside sits a touch above the water height
    constexpr F32 TEST_LIFT = 0.1f;
    // Terrain is drawn this deep under the water for the shallow-water fade (fsWaterRippleSimF.glsl
    // fades over the last metre)
    constexpr F32 SHALLOW_DEPTH = 1.5f;
    // Cleared value: nothing there (fsWaterObstacleF.glsl FS_OBST_FAR)
    constexpr F32 FAR_AWAY = 60000.f;

    // The mask reaches this share of the grid beyond it on every side, and is drawn again once
    // the camera has moved that far: the grid stays covered as it follows the camera
    constexpr S32 MARGIN_DIV = 8;
    // How often the geometry near the water is checked for changes (objects rezzed, moved,
    // deleted, a mesh loading in), and more often for a while after the ripples start over (a
    // teleport, arriving), while the scene loads. Each check walks the octrees only; the mask is
    // drawn only when what it would draw has changed.
    constexpr F64 CHECK_SECONDS = 5.0;
    constexpr F64 SETTLE_CHECK_SECONDS = 1.0;
    constexpr F64 SETTLE_SECONDS = 20.0;
    // Objects moving faster than this (m/s) make ripples (fswaterripples.cpp MOVING_SPEED); they
    // are no obstacle
    constexpr F32 MOVING_SPEED = 0.2f;
    // A big thin prim lying in the water surface is a fake water surface or a water tint, not a
    // wall: left out
    constexpr F32 WATER_PRIM_SIZE = 48.f;
    constexpr F32 WATER_PRIM_THICKNESS = 1.f;
    // At most this many faces a mask (the nearest are not picked: a cap against pathological
    // scenes, far above a busy harbour)
    constexpr size_t MAX_FACES = 8192;
    // A mask that could not be allocated is not tried again every frame
    constexpr F64 FAILED_RETRY_SECONDS = 30.0;
    // The water line strips (fsWaterObstacleG.glsl) reach this many cells to each side of the line,
    // so a line passing anywhere through a cell covers its centre
    constexpr F32 SLICE_HALF_WIDTH_CELLS = 0.75f;
    // FSRenderWaterRippleStats: seconds between the summary lines
    constexpr F64 STATS_SECONDS = 10.0;

    LLGLSLShader sProgram;
    LLRenderTarget* sMask = nullptr; // never destroyed at exit: released with the GL buffers
    bool sMaskValid = false;         // sMask holds a mask for the values below
    S32 sMaskRes = 0;                // texels across
    F64 sMaskCell = 0.0;             // texel size, metres
    S64 sMaskX = 0;                  // texel (0, 0), whole cells of global coordinates
    S64 sMaskY = 0;
    U64 sMaskHash = 0;               // what was drawn (see Draw)
    F64 sLastCheck = -1.0;
    F64 sSettleUntil = -1.0;
    F64 sFailedAt = -1.0;
    U64 sRegionHandle = 0;

    // FSRenderWaterRippleStats: what the gathering kept and left out
    struct GatherCounts
    {
        U32 mTerrainFaces = 0;
        U32 mStaticFaces = 0;
        U32 mLinksetFaces = 0;
        U32 mMoving = 0;         // objects left out: avatars, attachments, moving
        U32 mWaterPrims = 0;     // big thin prims in the water surface
        U32 mAlphaFaces = 0;     // alpha blended faces
        U32 mInvisibleFaces = 0;
        U32 mRigged = 0;
        U32 mNoGeometry = 0;     // faces without a buffer or triangles yet
        bool mCapped = false;    // MAX_FACES reached
    };

    // FSRenderWaterRippleStats: counters since the last summary line
    struct Stats
    {
        F64 mLastLog = -1.0;
        F64 mLastBuild = -1.0;   // when the mask was last drawn
        U32 mWaterUpdates = 0;   // FSWaterRipples::update calls (the water pass ran)
        U32 mUpdates = 0;        // update() calls with the obstacles on
        U32 mNoProgram = 0;      // returned: the mask program is not built
        U32 mNoRegion = 0;
        U32 mChecks = 0;         // geometry walks
        U32 mUnchanged = 0;      // walks that found nothing new
        U32 mBuilds = 0;
        U32 mAllocFailed = 0;
        U32 mSimSteps = 0;       // simulation passes that read the mask
        U32 mSimWithout = 0;     // simulation passes with the obstacles on but no mask to read
    };
    Stats sStats;

    bool statsOn()
    {
        static LLCachedControl<bool> stats(gSavedSettings, "FSRenderWaterRippleStats", false);
        return stats;
    }

    // One face to draw, gathered and drawn within the same update (the pointers never outlive it)
    struct Draw
    {
        LLFace* mFace;
        const LLMatrix4* mModel;
        F32 mWater;
        bool mTerrain;
    };

    // FNV-1a over what decides the mask's content: which faces, their buffers and ranges, their
    // matrices and the water heights
    void hashBytes(U64& hash, const void* data, size_t size)
    {
        const U8* bytes = static_cast<const U8*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 0x100000001b3ULL;
        }
    }

    void hashDraw(U64& hash, const Draw& draw)
    {
        const LLVertexBuffer* buffer = draw.mFace->getVertexBuffer();
        const U32 values[4] = { draw.mFace->getGeomIndex(), draw.mFace->getGeomCount(), draw.mFace->getIndicesCount(),
                                (U32)draw.mFace->getIndicesStart() };
        hashBytes(hash, &draw.mFace, sizeof(draw.mFace));
        hashBytes(hash, &buffer, sizeof(buffer));
        hashBytes(hash, values, sizeof(values));
        hashBytes(hash, draw.mModel->mMatrix, sizeof(draw.mModel->mMatrix));
        hashBytes(hash, &draw.mWater, sizeof(draw.mWater));
    }

    // Gathers the faces of the static geometry that reaches the water surface, and the terrain,
    // inside the mask's square. Prunes on each group's extents, so only the branches that reach
    // the square near the water are walked.
    class Gather : public OctreeTraveler
    {
    public:
        Gather(std::vector<Draw>& out, LLViewerRegion* region, const LLVector3& min, const LLVector3& max, bool terrain,
               GatherCounts& counts)
            : mOut(out), mRegion(region), mTerrain(terrain), mCounts(counts)
        {
            mWater = region->getWaterHeight();
            // Terrain counts down to the shallow-water fade, and all the way up (land is solid)
            const F32 low = mWater - (terrain ? SHALLOW_DEPTH : BAND);
            const F32 high = terrain ? F32_MAX : mWater + BAND;
            mMin.set(min.mV[VX], min.mV[VY], low);
            mMax.set(max.mV[VX], max.mV[VY], high);
        }

        void traverse(const OctreeNode* node) override
        {
            if (!node || node->getListenerCount() == 0)
            {
                return;
            }
            if (mOut.size() >= MAX_FACES)
            {
                mCounts.mCapped = true;
                return;
            }
            LLSpatialGroup* group = (LLSpatialGroup*)node->getListener(0);
            if (!group || group->isDead())
            {
                return;
            }
            if (!reaches(group->getExtents()))
            {
                return; // no part of this branch reaches the square near the water
            }
            OctreeTraveler::traverse(node);
        }

        void visit(const OctreeNode* branch) override
        {
            LLSpatialGroup* group = (LLSpatialGroup*)branch->getListener(0);
            if (!group)
            {
                return;
            }
            for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
            {
                LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
                if (!drawable || drawable->isDead())
                {
                    continue;
                }
                if (drawable->isSpatialBridge())
                {
                    addLinkset((LLSpatialBridge*)drawable);
                }
                else if (!drawable->isActive())
                {
                    if (mTerrain)
                    {
                        addFaces(drawable, &mRegion->mRenderMatrix);
                        continue;
                    }
                    if (!reaches(drawable->getSpatialExtents()))
                    {
                        continue;
                    }
                    if (!isSolidObject(drawable->getVObj()))
                    {
                        ++mCounts.mMoving;
                        continue;
                    }
                    if (isWaterPrim(drawable->getSpatialExtents()))
                    {
                        ++mCounts.mWaterPrims;
                        continue;
                    }
                    addFaces(drawable, &mRegion->mRenderMatrix);
                }
            }
        }

    private:
        bool reaches(const LLVector4a* ext) const
        {
            return ext[0][0] <= mMax.mV[VX] && ext[1][0] >= mMin.mV[VX] && ext[0][1] <= mMax.mV[VY] && ext[1][1] >= mMin.mV[VY] &&
                   ext[0][2] <= mMax.mV[VZ] && ext[1][2] >= mMin.mV[VZ];
        }

        static bool isWaterPrim(const LLVector4a* ext)
        {
            return ext[1][0] - ext[0][0] > WATER_PRIM_SIZE && ext[1][1] - ext[0][1] > WATER_PRIM_SIZE &&
                   ext[1][2] - ext[0][2] < WATER_PRIM_THICKNESS;
        }

        // A prim of a static object, not something that makes ripples of its own
        static bool isSolidObject(const LLViewerObject* object)
        {
            if (!object || object->isDead() || object->isAvatar() || object->isAttachment() || object->isHUDAttachment() ||
                object->getPCode() != LL_PCODE_VOLUME)
            {
                return false;
            }
            const LLViewerObject* root = object->getRootEdit();
            return root && !root->isAvatar() && !root->isAttachment() && !(root->getVelocity().length() > MOVING_SPEED);
        }

        // An object that moves as one (physical, or recently moved): its whole linkset when its box
        // reaches the water, unless it is moving now (then it makes a wake instead)
        void addLinkset(LLSpatialBridge* bridge)
        {
            LLDrawable* root = bridge->mDrawable;
            if (mTerrain || !root || root->isDead())
            {
                return;
            }
            const LLVector4a* ext = bridge->getSpatialExtents();
            if (!reaches(ext))
            {
                return;
            }
            if (!isSolidObject(root->getVObj()))
            {
                ++mCounts.mMoving;
                return;
            }
            if (isWaterPrim(ext))
            {
                ++mCounts.mWaterPrims;
                return;
            }
            addFaces(root, &root->getRenderMatrix());
            for (LLViewerObject* child : root->getVObj()->getChildren())
            {
                LLDrawable* drawable = child ? child->mDrawable.get() : nullptr;
                if (drawable && !drawable->isDead() && child->getPCode() == LL_PCODE_VOLUME && !child->isAvatar())
                {
                    addFaces(drawable, &drawable->getRenderMatrix());
                }
            }
        }

        void addFaces(LLDrawable* drawable, const LLMatrix4* model)
        {
            if (drawable->isState(LLDrawable::RIGGED))
            {
                ++mCounts.mRigged;
                return; // drawn skinned, not from these buffers
            }
            const S32 faces = drawable->getNumFaces();
            for (S32 i = 0; i < faces; ++i)
            {
                if (mOut.size() >= MAX_FACES)
                {
                    mCounts.mCapped = true;
                    return;
                }
                LLFace* face = drawable->getFace(i);
                if (!face)
                {
                    continue;
                }
                if (face->isState(LLFace::RIGGED))
                {
                    ++mCounts.mRigged;
                    continue;
                }
                if (!face->getVertexBuffer() || face->getIndicesCount() == 0 || face->getGeomCount() == 0)
                {
                    ++mCounts.mNoGeometry;
                    continue;
                }
                if (!mTerrain)
                {
                    // Glass, alpha-blended water surfaces and invisible prims: not walls you see
                    if (face->isInAlphaPool())
                    {
                        ++mCounts.mAlphaFaces;
                        continue;
                    }
                    const LLTextureEntry* te = face->getTextureEntry();
                    if (te && te->getColor().mV[VALPHA] < 0.05f)
                    {
                        ++mCounts.mInvisibleFaces;
                        continue;
                    }
                }
                mOut.push_back({ face, model, mWater, mTerrain });
                if (mTerrain)
                {
                    ++mCounts.mTerrainFaces;
                }
                else if (model == &mRegion->mRenderMatrix)
                {
                    ++mCounts.mStaticFaces;
                }
                else
                {
                    ++mCounts.mLinksetFaces;
                }
            }
        }

        std::vector<Draw>& mOut;
        LLViewerRegion* mRegion;
        bool mTerrain;
        GatherCounts& mCounts;
        F32 mWater;
        LLVector3 mMin;
        LLVector3 mMax;
    };

    void gather(std::vector<Draw>& out, const LLVector3& min, const LLVector3& max, GatherCounts& counts)
    {
        out.clear();
        counts = GatherCounts();
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (!region)
            {
                continue;
            }
            static const U32 partitions[] =
            {
                LLViewerRegion::PARTITION_TERRAIN,
                LLViewerRegion::PARTITION_VOLUME,
                LLViewerRegion::PARTITION_BRIDGE,
            };
            for (U32 type : partitions)
            {
                LLSpatialPartition* part = region->getSpatialPartition(type);
                if (part && part->mOctree)
                {
                    Gather walk(out, region, min, max, type == LLViewerRegion::PARTITION_TERRAIN, counts);
                    walk.traverse(part->mOctree);
                }
            }
        }
    }

    void releaseMask()
    {
        if (sMask)
        {
            sMask->release();
        }
        sMaskValid = false;
    }

    bool ensureMask(S32 res, F64 now)
    {
        if (sMask && sMask->isComplete() && (S32)sMask->getWidth() == res)
        {
            return true;
        }
        if (sFailedAt >= 0.0 && now - sFailedAt < FAILED_RETRY_SECONDS)
        {
            return false;
        }
        releaseMask();
        if (!sMask)
        {
            sMask = new LLRenderTarget();
        }
        if (!sMask->allocate(res, res, GL_RGBA16F, false))
        {
            LL_WARNS("Render") << "Water ripple obstacles: a " << res << "x" << res << " mask could not be allocated" << LL_ENDL;
            releaseMask();
            sFailedAt = now;
            return false;
        }
        sFailedAt = -1.0;
        return true;
    }

    // Draws the gathered faces into the mask. Every texel is cleared first (AMD doesn't zero new
    // targets), so a mask is complete or not drawn at all.
    void drawMask(const std::vector<Draw>& draws, S32 res, F64 cell, S64 origin_x, S64 origin_y)
    {
        static LLStaticHashedString s_model("fs_obst_model");
        static LLStaticHashedString s_view("fs_obst_view");
        static LLStaticHashedString s_water("fs_obst_water");

        LL_PROFILE_GPU_ZONE("water obstacle mask");

        gGL.flush();
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable cull(GL_CULL_FACE);   // both sides: which way a surface faces is the answer
        LLGLDisable scissor(GL_SCISSOR_TEST);
        LLGLEnable blend(GL_BLEND);
        gGL.setColorMask(true, true);

        sMask->bindTarget();
        const GLfloat far_away[4] = { FAR_AWAY, FAR_AWAY, FAR_AWAY, FAR_AWAY };
        glClearBufferfv(GL_COLOR, 0, far_away);

        // Each channel keeps the nearest surface of its kind (fsWaterObstacleF.glsl)
        glBlendEquationSeparate(GL_MIN, GL_MIN);

        // The mask's corner in agent space (z unused)
        const LLVector3 corner = gAgent.getPosAgentFromGlobal(LLVector3d((F64)origin_x * cell, (F64)origin_y * cell, 0.0));
        sProgram.bind();
        sProgram.uniform4f(s_view, corner.mV[VX], corner.mV[VY], (F32)(2.0 / ((F64)res * cell)), (F32)cell * SLICE_HALF_WIDTH_CELLS);

        const LLMatrix4* model = nullptr;
        F32 water = F32_MAX;
        bool terrain = false;
        for (const Draw& draw : draws)
        {
            if (draw.mModel != model)
            {
                model = draw.mModel;
                sProgram.uniformMatrix4fv(s_model, 1, GL_FALSE, (const GLfloat*)model->mMatrix);
            }
            if (draw.mWater != water || draw.mTerrain != terrain)
            {
                water = draw.mWater;
                terrain = draw.mTerrain;
                sProgram.uniform4f(s_water, water, TEST_LIFT, terrain ? 1.f : 0.f, SHALLOW_DEPTH);
            }
            LLFace* face = draw.mFace;
            LLVertexBuffer* buffer = face->getVertexBuffer();
            buffer->setBuffer();
            buffer->drawRange(LLRender::TRIANGLES, face->getGeomIndex(), face->getGeomIndex() + face->getGeomCount() - 1,
                              face->getIndicesCount(), face->getIndicesStart());
        }

        sProgram.unbind();
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD); // what the rest of the viewer expects
        sMask->flush();
    }

    // FSRenderWaterRippleStats only: reads the mask back (a stall, once per redraw) and counts its
    // cells. solid: what the simulation treats as solid; line: of those, marked by a water line
    // strip; shallow: open cells in the shallow-water fade.
    void countMask(S32 res, U32& solid, U32& line, U32& shallow)
    {
        solid = line = shallow = 0;
        static std::vector<F32> texels;
        texels.resize((size_t)res * (size_t)res * 4);
        sMask->bindTarget();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, res, res, GL_RGBA, GL_FLOAT, texels.data());
        sMask->flush();
        for (size_t i = 0; i < texels.size(); i += 4)
        {
            const bool on_line = texels[i + 3] < 0.5f;
            if (texels[i] < texels[i + 1] || on_line)
            {
                ++solid;
                if (on_line)
                {
                    ++line;
                }
            }
            else if (texels[i + 2] < 1.f)
            {
                ++shallow;
            }
        }
    }
}

bool FSWaterObstacles::enabled()
{
    static LLCachedControl<bool> ripples(gSavedSettings, "FSRenderWaterRipples", false);
    static LLCachedControl<bool> obstacles(gSavedSettings, "FSRenderWaterRippleObstacles", false);
    return ripples && obstacles;
}

void FSWaterObstacles::loadShaders(S32 shader_level)
{
    sProgram.unload();
    if (!enabled() || shader_level <= 0)
    {
        release();
        return;
    }

    sProgram.mName = "FS Water Ripple Obstacles";
    sProgram.mShaderFiles.clear();
    sProgram.clearPermutations();
    sProgram.mShaderFiles.push_back(std::make_pair("environment/fsWaterObstacleV.glsl", GL_VERTEX_SHADER));
    sProgram.mShaderFiles.push_back(std::make_pair("environment/fsWaterObstacleG.glsl", GL_GEOMETRY_SHADER));
    sProgram.mShaderFiles.push_back(std::make_pair("environment/fsWaterObstacleF.glsl", GL_FRAGMENT_SHADER));
    sProgram.mShaderLevel = shader_level;
    if (!sProgram.createShader())
    {
        LL_WARNS("ShaderLoading") << "FS Water Ripple Obstacles failed to build, ripples pass through objects" << LL_ENDL;
        sProgram.unload();
    }
    sMaskValid = false; // drawn again with the new program
}

void FSWaterObstacles::update(S32 res, F64 cell, const LLVector3d& camera, F64 now, bool force)
{
    if (!enabled())
    {
        return;
    }
    ++sStats.mUpdates;
    if (!sProgram.isComplete() || res <= 0 || !(cell > 0.0))
    {
        ++sStats.mNoProgram;
        return;
    }
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (!agent_region)
    {
        ++sStats.mNoRegion;
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    const S32 margin = res / MARGIN_DIV;
    const S32 mask_res = res + 2 * margin;
    const S64 cam_x = (S64)floor(camera.mdV[VX] / cell);
    const S64 cam_y = (S64)floor(camera.mdV[VY] / cell);

    // Drawn again: no mask yet, another grid, the camera close to an eighth of the grid away from
    // where the mask was centred (the grid is centred on the camera to within a cell, whatever the
    // surface current), or another region (a teleport)
    const S32 reach = llmax(margin - 2, 1);
    // Why it is drawn again, for FSRenderWaterRippleStats
    const char* reason = nullptr;
    if (!sMaskValid)
    {
        reason = "no mask yet";
    }
    else if (sMaskRes != mask_res || sMaskCell != cell)
    {
        reason = "grid changed";
    }
    else if (llabs(cam_x - (sMaskX + mask_res / 2)) > reach || llabs(cam_y - (sMaskY + mask_res / 2)) > reach)
    {
        reason = "camera moved";
    }
    bool redraw = reason != nullptr;
    if (agent_region->getHandle() != sRegionHandle)
    {
        sRegionHandle = agent_region->getHandle();
        redraw = true;
        if (!reason)
        {
            reason = "region changed";
        }
        sSettleUntil = now + SETTLE_SECONDS;
    }
    if (force)
    {
        sSettleUntil = now + SETTLE_SECONDS;
    }
    const F64 interval = now < sSettleUntil ? SETTLE_CHECK_SECONDS : CHECK_SECONDS;
    const bool check = redraw || force || sLastCheck < 0.0 || now - sLastCheck >= interval || now < sLastCheck;
    if (!check)
    {
        return;
    }
    sLastCheck = now;
    ++sStats.mChecks;
    LLTimer timer;

    S64 origin_x = sMaskX;
    S64 origin_y = sMaskY;
    if (redraw)
    {
        origin_x = cam_x - mask_res / 2;
        origin_y = cam_y - mask_res / 2;
    }

    // The square the mask covers, agent space
    const LLVector3 min = gAgent.getPosAgentFromGlobal(LLVector3d((F64)origin_x * cell, (F64)origin_y * cell, 0.0));
    const F32 size = (F32)((F64)mask_res * cell);
    const LLVector3 max(min.mV[VX] + size, min.mV[VY] + size, 0.f);

    static std::vector<Draw> draws;
    GatherCounts counts;
    gather(draws, min, max, counts);
    U64 hash = 0xcbf29ce484222325ULL;
    for (const Draw& draw : draws)
    {
        hashDraw(hash, draw);
    }
    if (!redraw && hash == sMaskHash)
    {
        draws.clear();
        ++sStats.mUnchanged;
        return; // nothing near the water changed
    }
    if (!reason)
    {
        reason = force ? "ripples started over, geometry changed" : "geometry changed";
    }

    if (!ensureMask(mask_res, now))
    {
        draws.clear();
        ++sStats.mAllocFailed;
        return;
    }
    drawMask(draws, mask_res, cell, origin_x, origin_y);
    ++sStats.mBuilds;
    sStats.mLastBuild = now;
    if (statsOn())
    {
        const F64 ms = timer.getElapsedTimeF64() * 1000.0;
        U32 solid = 0;
        U32 line = 0;
        U32 shallow = 0;
        countMask(mask_res, solid, line, shallow);
        LL_INFOS("FSWaterRipples") << "Obstacle mask drawn (" << reason << "): " << mask_res << "x" << mask_res << " cells of "
                                   << llformat("%.3f", cell) << " m; " << draws.size() << " faces (" << counts.mStaticFaces
                                   << " static, " << counts.mLinksetFaces << " in linksets, " << counts.mTerrainFaces
                                   << " terrain" << (counts.mCapped ? ", capped" : "") << "); left out: " << counts.mMoving
                                   << " avatars, attachments or moving objects, " << counts.mWaterPrims << " water prims, "
                                   << counts.mAlphaFaces << " alpha faces, " << counts.mInvisibleFaces << " invisible faces, "
                                   << counts.mRigged << " rigged, " << counts.mNoGeometry << " faces without geometry; "
                                   << solid << " cells solid (" << line << " on a water line), " << shallow
                                   << " in shallow water; " << llformat("%.2f", ms) << " ms to gather and draw" << LL_ENDL;
    }
    draws.clear();

    sMaskValid = true;
    sMaskRes = mask_res;
    sMaskCell = cell;
    sMaskX = origin_x;
    sMaskY = origin_y;
    sMaskHash = hash;
}

void FSWaterObstacles::noteSimulation(bool with_mask)
{
    if (with_mask)
    {
        ++sStats.mSimSteps;
    }
    else
    {
        ++sStats.mSimWithout;
    }
}

void FSWaterObstacles::logStats(F64 now)
{
    ++sStats.mWaterUpdates;
    if (!statsOn())
    {
        sStats.mLastLog = -1.0;
        return;
    }
    if (sStats.mLastLog < 0.0 || now < sStats.mLastLog)
    {
        sStats.mLastLog = now;
        return;
    }
    if (now - sStats.mLastLog < STATS_SECONDS)
    {
        return;
    }
    static LLCachedControl<bool> ripples(gSavedSettings, "FSRenderWaterRipples", false);
    static LLCachedControl<bool> obstacles(gSavedSettings, "FSRenderWaterRippleObstacles", false);
    const bool active = enabled() && sProgram.isComplete() && sMaskValid && sMask && sMask->isComplete();
    LL_INFOS("FSWaterRipples") << "Obstacles " << (active ? "active" : "not active") << " (ripples " << (ripples ? "on" : "off")
                               << ", obstacles " << (obstacles ? "on" : "off") << ", mask program "
                               << (sProgram.isComplete() ? "built" : "not built") << ", mask "
                               << (sMaskValid ? llformat("%dx%d", sMaskRes, sMaskRes) : std::string("none")) << "); last drawn "
                               << (sStats.mLastBuild >= 0.0 ? llformat("%.1f s ago", now - sStats.mLastBuild) : std::string("never"))
                               << "; in the last " << llformat("%.0f", now - sStats.mLastLog) << " s: " << sStats.mWaterUpdates
                               << " water passes, " << sStats.mUpdates << " mask updates (" << sStats.mNoProgram
                               << " without the program, " << sStats.mNoRegion << " without a region), " << sStats.mChecks
                               << " geometry checks (" << sStats.mUnchanged << " unchanged), " << sStats.mBuilds << " drawn, "
                               << sStats.mAllocFailed << " allocations failed; " << sStats.mSimSteps
                               << " simulation passes read the mask, " << sStats.mSimWithout << " had none" << LL_ENDL;
    const F64 last_build = sStats.mLastBuild;
    sStats = Stats();
    sStats.mLastBuild = last_build;
    sStats.mLastLog = now;
}

LLRenderTarget* FSWaterObstacles::mask(F64 cell, S64& origin_x, S64& origin_y)
{
    if (!sMaskValid || !sMask || !sMask->isComplete() || sMaskCell != cell)
    {
        return nullptr;
    }
    origin_x = sMaskX;
    origin_y = sMaskY;
    return sMask;
}

void FSWaterObstacles::release()
{
    releaseMask();
    sMaskRes = 0;
    sMaskCell = 0.0;
    sMaskHash = 0;
    sLastCheck = -1.0;
    sSettleUntil = -1.0;
    sFailedAt = -1.0;
    sRegionHandle = 0;
}
