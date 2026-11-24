/**
 * @file fssoftparticles.cpp
 * @brief Soft particles: particles fade where they meet the scene (FSRenderSoftParticles)
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

#include "fssoftparticles.h"
#include "fsreversez.h"

#include "lldrawpool.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercontrol.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    U32 sDepthFrame = U32_MAX;  // frame of the last pre-alpha depth copy of the main view

    // the pass's values, from beginPass
    LLVector4 sParams;          // see fs_soft_particle in fsSoftParticlesF.glsl, x and w set per draw
    F32 sWaterInvDistance = 0.f;

    const LLStaticHashedString& params_name()
    {
        static LLStaticHashedString name("fs_soft_particle");
        return name;
    }

    // whether this view draws water (as LLDrawPoolWater decides it, see fs_water_will_draw there)
    bool water_drawn()
    {
        LLDrawPool* pool = gPipeline.findPool(LLDrawPool::POOL_WATER);
        return pool && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_WATER) &&
               !static_cast<LLFacePool*>(pool)->mDrawFace.empty() && pool->getNumPostDeferredPasses() > 0;
    }
}

bool FSSoftParticles::enabled()
{
    static LLCachedControl<bool> soft(gSavedSettings, "FSRenderSoftParticles", false);
    return soft;
}

void FSSoftParticles::noteDepthTaken()
{
    sDepthFrame = LLFrameTimer::getFrameCount();
}

bool FSSoftParticles::beginPass()
{
    if (!enabled() || sDepthFrame != LLFrameTimer::getFrameCount() || gCubeSnapshot || LLPipeline::sRenderingHUDs ||
        LLPipeline::sImpostorRender || LLPipeline::sReflectionRender || gPipeline.mRT != &gPipeline.mMainRT ||
        LLPipeline::isAvatarOnlyRender())
    {
        return false;
    }
    const LLRenderTarget& depth = gPipeline.mFSTagDepth;
    if (!depth.isComplete() || depth.getWidth() != gPipeline.mRT->deferredScreen.getWidth() ||
        depth.getHeight() != gPipeline.mRT->deferredScreen.getHeight())
    {
        return false;
    }

    static LLCachedControl<F32> distance(gSavedSettings, "FSRenderSoftParticlesDistance", 0.5f);
    const F32 inv_distance = 1.f / llclamp((F32)distance, 0.05f, 10.f);
    sWaterInvDistance = water_drawn() ? inv_distance : 0.f;

    // the camera's projection as the pass draws with it: [2][2] and [3][2] turn window depth into distance
    // (FSRenderReverseZ: the reversed depth row, which fsSoftParticlesF.glsl reads as such)
    const glm::mat4 proj = FSReverseZ::depthProjection(get_current_projection());
    sParams.set(inv_distance, proj[2][2], proj[3][2], 0.f);
    return true;
}

void FSSoftParticles::apply(LLGLSLShader& shader, bool particle, bool additive)
{
    if (shader.getUniformLocation(params_name()) < 0)
    {
        return; // not built with FS_SOFT_PARTICLES
    }
    if (!particle)
    {
        shader.uniform4f(params_name(), 0.f, 0.f, 0.f, 0.f);
        return;
    }

    // The last texture unit, which no program's own samplers reach. Not the first one after this
    // program's own: the depth stays bound after the draw, and the next alpha program (its unit count
    // differs, by the SSR samplers for one) could have bound a texture for the whole pass on that unit,
    // such as the probes' shCoeffs, and read the depth in its place: red, (depth, 0, 0) as lighting.
    static LLStaticHashedString s_depth("fs_soft_depth");
    static LLStaticHashedString s_water("fs_soft_particle_water");
    const S32 unit = llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS) - 1;
    if (shader.getUniformLocation(s_depth) < 0 || unit < shader.mActiveTextureChannels)
    {
        LL_WARNS_ONCE("Rendering") << "FSRenderSoftParticles: " << shader.mName << " has no free texture unit for the scene "
                                   << "depth, particles drawn with it keep hard edges" << LL_ENDL;
        shader.uniform4f(params_name(), 0.f, 0.f, 0.f, 0.f);
        return;
    }
    shader.uniform1i(s_depth, unit);
    gGL.getTexUnit(unit)->bind(&gPipeline.mFSTagDepth, true);
    shader.uniform4f(params_name(), sParams.mV[VX], sParams.mV[VY], sParams.mV[VZ], additive ? 1.f : 0.f);
    shader.uniform1f(s_water, sWaterInvDistance);
}
