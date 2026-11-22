/**
 * @file fsshadowdepthfit.cpp
 * @brief Sun shadow maps fitted to the depth range in view (FSRenderShadowFitDepth)
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

// How the range is kept steady (getRange)
//
// The read back is one to three frames old. A range that is too short leaves the far end of the
// view without shadows, which shows at once; one that is too long only makes the shadows a little
// softer. So the range used grows at once to what the latest read back needs, and shrinks toward
// it smoothly (time constant 0.25 s): an avatar walking through the view, or a door opening,
// widens it straight away and lets it tighten again over a second, instead of pumping with every
// frame. It is also rounded outward to steps of about 5 %, so a still camera gives exactly the same
// splits every frame: texel snapping, the far map throttle and the static far map cache keep
// working, and nothing shimmers.
// The read back carries the camera it was taken with. When the camera has since moved or turned
// more than a little (a fast turn, a teleport), what it shows now may lie outside that range, so
// the frame uses the range it would have without the fit, which is again a growth and so
// immediate, and the fit tightens again once the camera settles.

#include "llviewerprecompiledheaders.h"

#include "fsshadowdepthfit.h"
#include "fsreversez.h"

#include "fsglstream.h"

#include "llcamera.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"

#include <glm/gtc/type_ptr.hpp>

#include <atomic>
#include <cmath>
#include <vector>

extern bool gCubeSnapshot;
extern bool gSnapshot;

namespace
{
    constexpr S32 BLOCK = 16;               // texels per side one output texel covers (FS_FIT_BLOCK in the shader)
    constexpr U32 MAX_AGE_FRAMES = 30;      // an older read back is not used
    constexpr F32 SHRINK_SECONDS = 0.25f;   // time constant of the range's shrinking
    constexpr F32 NEAR_MARGIN = 0.8f;       // the near end is pulled in by this factor
    constexpr F32 FAR_MARGIN = 1.15f;       // and the far end pushed out by this factor
    constexpr F32 FAR_PAD = 0.5f;           // plus this many metres
    constexpr F32 MAX_NEAR = 4.f;           // as the splits without the fit
    constexpr F32 MOVE_METRES = 1.f;        // camera moved further than this (plus 5 % of the far end)
    constexpr F32 TURN_COS = 0.990f;        // or turned more than about 8 degrees: use the full range
    constexpr F32 CUT_FADE = 0.1f;          // share of a cut range the far fade takes (fadeLength)

    LLGLSLShader sFirstProgram;     // depth buffer -> nearest and farthest distance per block
    LLGLSLShader sReduceProgram;    // the same from the level before
    bool sProgramsTried = false;    // built, or failed, since the last shader load

    const LLStaticHashedString sInvProj("fs_fit_inv_proj");
    const LLStaticHashedString sSrcSize("fs_fit_src_size");

    struct Level
    {
        U32 mTexture = 0;
        U32 mFBO = 0;
        U32 mWidth = 0;
        U32 mHeight = 0;
    };
    std::vector<Level> sLevels;     // the last one is 1x1
    U32 sSourceWidth = 0;
    U32 sSourceHeight = 0;

    // Read back. The GL thread (the render thread, or the main thread without it) owns the fence
    // and writes sResult before it publishes RB_READY.
    enum { RB_IDLE = 0, RB_PENDING, RB_READY };
    std::atomic<int> sState{ RB_IDLE };
    GLsync sFence = nullptr;
    F32 sResult[2] = { 0.f, 0.f };   // nearest, farthest: the RG32F texel as it is stored
    U32 sPBO = 0;

    // main thread: the camera of the frame whose depth is being read back
    LLVector3 sPendingOrigin;
    LLVector3 sPendingAt;
    U32 sPendingFrame = 0;

    // main thread: the latest result
    bool sHave = false;
    F32 sNear = 0.f;
    F32 sFar = 0.f;
    LLVector3 sOrigin;
    LLVector3 sAt;
    U32 sFrame = 0;

    // main thread: the range in use
    bool sSmoothValid = false;
    F32 sSmoothNear = 0.f;
    F32 sSmoothFar = 0.f;
    F64 sLastTime = 0.0;
    bool sRangeCut = false;
    F32 sCutRatio = 1.f;            // the range in use over the range without the fit

    // GL thread, in stream order right after the copy into the pack buffer
    void gl_fence(U64)
    {
        if (sFence)
        {
            glDeleteSync(sFence);
        }
        sFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sFence)
        {
            sState.store(RB_IDLE, std::memory_order_release);
        }
    }

    // GL thread: copies the result out once the GPU has passed the fence. The pack buffer binding
    // is put back as found: the main thread's stream tracks it.
    void gl_poll(U64 pbo)
    {
        if (sState.load(std::memory_order_relaxed) != RB_PENDING)
        {
            return;
        }
        // No fence: this poll was queued for a read back that is done, and the main thread has since
        // started a new one whose gl_fence runs after it. Setting IDLE here dropped that new one
        // (a failed fence sets IDLE in gl_fence itself).
        if (!sFence)
        {
            return;
        }
        const GLenum status = glClientWaitSync(sFence, 0, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
        {
            if (status == GL_WAIT_FAILED)
            {
                glDeleteSync(sFence);
                sFence = nullptr;
                sState.store(RB_IDLE, std::memory_order_release);
            }
            return;
        }
        glDeleteSync(sFence);
        sFence = nullptr;

        GLint prev = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(sResult), sResult);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev);
        sState.store(RB_READY, std::memory_order_release);
    }

    // GL thread
    void gl_release(U64)
    {
        if (sFence)
        {
            glDeleteSync(sFence);
            sFence = nullptr;
        }
        sState.store(RB_IDLE, std::memory_order_release);
    }

    // Main thread: takes a finished read back, keeps a pending one polled
    void poll_readback()
    {
        const int state = sState.load(std::memory_order_acquire);
        if (state == RB_READY)
        {
            sNear = sResult[0];
            sFar = sResult[1];
            sOrigin = sPendingOrigin;
            sAt = sPendingAt;
            sFrame = sPendingFrame;
            sHave = true;
            sState.store(RB_IDLE, std::memory_order_relaxed);
        }
        else if (state == RB_PENDING)
        {
            FSGLStream::recordCall(&gl_poll, (U64)sPBO);
        }
    }

    void release_levels()
    {
        for (Level& level : sLevels)
        {
            if (level.mFBO)
            {
                glDeleteFramebuffers(1, &level.mFBO);
            }
            if (level.mTexture)
            {
                // don't leave a texture unit pointing at a deleted name
                for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; ++i)
                {
                    LLTexUnit* unit = gGL.getTexUnit(i);
                    if (unit && unit->getCurrTexture() == level.mTexture)
                    {
                        unit->unbind(LLTexUnit::TT_TEXTURE);
                    }
                }
                glDeleteTextures(1, &level.mTexture);
            }
        }
        sLevels.clear();
        sSourceWidth = 0;
        sSourceHeight = 0;
    }

    void release_readback()
    {
        FSGLStream::recordCall(&gl_release, 0);
        if (sPBO)
        {
            glDeleteBuffers(1, &sPBO);
            sPBO = 0;
        }
        sHave = false;
        sSmoothValid = false;
        sRangeCut = false;
    }

    // The chain of reduction targets for a source of this size, down to 1x1
    bool allocate_levels(U32 width, U32 height)
    {
        release_levels();

        U32 w = width;
        U32 h = height;
        do
        {
            w = (w + BLOCK - 1) / BLOCK;
            h = (h + BLOCK - 1) / BLOCK;
            Level level;
            level.mWidth = w;
            level.mHeight = h;
            sLevels.push_back(level);
        } while (w > 1 || h > 1);

        LLTexUnit* unit = gGL.getTexUnit(0);
        bool ok = true;
        clear_glerror();
        for (Level& level : sLevels)
        {
            glGenTextures(1, &level.mTexture);
            glGenFramebuffers(1, &level.mFBO);
            if (!level.mTexture || !level.mFBO)
            {
                ok = false;
                break;
            }
            unit->bindManual(LLTexUnit::TT_TEXTURE, level.mTexture, true);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, level.mWidth, level.mHeight, 0, GL_RG, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            unit->unbind(LLTexUnit::TT_TEXTURE);
            glBindFramebuffer(GL_FRAMEBUFFER, level.mFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level.mTexture, 0);
            ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (!ok)
            {
                break;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        ok = ok && glGetError() == GL_NO_ERROR;

        if (!ok)
        {
            LL_WARNS("Render") << "Shadow depth fit targets for " << width << "x" << height << " could not be allocated" << LL_ENDL;
            release_levels();
            return false;
        }
        sSourceWidth = width;
        sSourceHeight = height;
        return true;
    }

    // Built on first use: the setting doesn't rebuild the shaders
    bool programs_ready()
    {
        if (sProgramsTried)
        {
            return sFirstProgram.isComplete() && sReduceProgram.isComplete();
        }
        sProgramsTried = true;

        const S32 level = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_DEFERRED);
        LLGLSLShader* shaders[2] = { &sFirstProgram, &sReduceProgram };
        for (S32 i = 0; i < 2; ++i)
        {
            LLGLSLShader* shader = shaders[i];
            shader->mName = i == 0 ? "FS Shadow Depth Fit First" : "FS Shadow Depth Fit Reduce";
            shader->mShaderFiles.clear();
            shader->clearPermutations();
            if (i == 0)
            {
                shader->addPermutation("FS_DEPTH_FIT_FIRST", "1");
            }
            shader->mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
            shader->mShaderFiles.push_back(std::make_pair("deferred/fsShadowDepthFitF.glsl", GL_FRAGMENT_SHADER));
            shader->mShaderLevel = level;
            if (!shader->createShader())
            {
                LL_WARNS("ShaderLoading") << shader->mName << " failed to build, sun shadows keep the usual splits" << LL_ENDL;
                sFirstProgram.unload();
                sReduceProgram.unload();
                return false;
            }
        }
        return true;
    }

    void draw_level(const Level& level, LLVertexBuffer* screen_triangle)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, level.mFBO);
        glViewport(0, 0, level.mWidth, level.mHeight);
        screen_triangle->setBuffer();
        screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    // rounded up (down) to steps of 1/16 of the power of two below x: about 3 to 6 %
    F32 quantize_up(F32 x)
    {
        const F32 step = powf(2.f, floorf(log2f(llmax(x, 0.01f))) - 4.f);
        return ceilf(x / step) * step;
    }

    F32 quantize_down(F32 x)
    {
        const F32 step = powf(2.f, floorf(log2f(llmax(x, 0.01f))) - 4.f);
        return floorf(x / step) * step;
    }
}

bool FSShadowDepthFit::enabled()
{
    static LLCachedControl<bool> fit(gSavedSettings, "FSRenderShadowFitDepth", false);
    static LLCachedControl<S32> shadow_detail(gSavedSettings, "RenderShadowDetail", 2);
    return fit && shadow_detail > 0;
}

void FSShadowDepthFit::loadShaders()
{
    sFirstProgram.unload();
    sReduceProgram.unload();
    sProgramsTried = false;
}

void FSShadowDepthFit::reduce(LLRenderTarget& depth_target, LLVertexBuffer* screen_triangle)
{
    poll_readback();

    if (!enabled())
    {
        if (!sLevels.empty() || sPBO || sHave)
        {
            release();
        }
        return;
    }
    if (gCubeSnapshot || !screen_triangle || !depth_target.getDepth() || !programs_ready())
    {
        return;
    }

    const U32 width = depth_target.getWidth();
    const U32 height = depth_target.getHeight();
    if (!width || !height)
    {
        return;
    }
    if ((width != sSourceWidth || height != sSourceHeight || sLevels.empty()) && !allocate_levels(width, height))
    {
        return;
    }

    LL_PROFILE_GPU_ZONE("shadow depth fit");
    gGL.flush();

    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);

    // The first level from the depth buffer, which was drawn with the current projection
    sFirstProgram.bind();
    S32 channel = sFirstProgram.enableTexture(LLShaderMgr::DEFERRED_DEPTH);
    bool reduced = channel > -1;
    if (reduced)
    {
        gGL.getTexUnit(channel)->bind(&depth_target, true);
    }
    // the inverse of the projection the depth was written with (FSRenderReverseZ: reversed even here,
    // inside the shadow passes, which draw with the standard convention)
    const glm::mat4 inv_proj = glm::inverse(FSReverseZ::depthProjection(get_current_projection()));
    sFirstProgram.uniformMatrix4fv(sInvProj, 1, GL_FALSE, glm::value_ptr(inv_proj));
    sFirstProgram.uniform2i(sSrcSize, (GLint)width, (GLint)height);
    draw_level(sLevels[0], screen_triangle);
    if (reduced)
    {
        sFirstProgram.disableTexture(LLShaderMgr::DEFERRED_DEPTH);
    }
    sFirstProgram.unbind();

    // The coarser levels, each from the one before
    if (sLevels.size() > 1)
    {
        sReduceProgram.bind();
        channel = sReduceProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        reduced = reduced && channel > -1;
        if (channel > -1)
        {
            LLTexUnit* unit = gGL.getTexUnit(channel);
            for (size_t i = 1; i < sLevels.size(); ++i)
            {
                unit->bindManual(LLTexUnit::TT_TEXTURE, sLevels[i - 1].mTexture, true);
                sReduceProgram.uniform2i(sSrcSize, (GLint)sLevels[i - 1].mWidth, (GLint)sLevels[i - 1].mHeight);
                draw_level(sLevels[i], screen_triangle);
            }
            unit->unbind(LLTexUnit::TT_TEXTURE);
        }
        sReduceProgram.unbind();
    }

    // Read the 1x1 result back, unless the last read back is still on its way. The last level's
    // target is still bound, so glReadPixels reads it. In the texture's own format (RG, float): read
    // as RGBA it needs a format conversion, which a driver may do outside the plain asynchronous copy
    // path, by waiting for the GPU to finish all the work before it (a GPU left idle shows up in the
    // "shadow depth fit" GPU zone: 1.5 to 2.5 ms on an AMD RX 7900 XTX, 0.14 ms on NVIDIA).
    if (sState.load(std::memory_order_acquire) == RB_IDLE && reduced)
    {
        if (!sPBO)
        {
            glGenBuffers(1, &sPBO);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, sPBO);
            glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(sResult), nullptr, GL_STREAM_READ);
        }
        else
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, sPBO);
        }
        //glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, nullptr);
        glReadPixels(0, 0, 1, 1, GL_RG, GL_FLOAT, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        sState.store(RB_PENDING, std::memory_order_relaxed);
        FSGLStream::recordCall(&gl_fence, 0);

        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        sPendingOrigin = camera->getOrigin();
        sPendingAt = camera->getAtAxis();
        sPendingFrame = LLFrameTimer::getFrameCount();
    }

    // back to whatever target the pipeline had bound
    glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    if (LLRenderTarget::sCurFBO)
    {
        glViewport(0, 0, LLRenderTarget::sCurResX, LLRenderTarget::sCurResY);
    }
    else
    {
        glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
    }
}

bool FSShadowDepthFit::getRange(const LLCamera& camera, F32 cpu_near, F32 cpu_far, F32& near_out, F32& far_out)
{
    sRangeCut = false;
    sCutRatio = 1.f;
    if (!enabled() || gSnapshot || !(cpu_far > cpu_near))
    {
        sSmoothValid = false;
        return false;
    }

    poll_readback();

    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = sSmoothValid ? llclamp((F32)(now - sLastTime), 0.f, 0.25f) : 0.f;
    sLastTime = now;

    const U32 frame = LLFrameTimer::getFrameCount();
    // nothing but sky in view gives near > far
    const bool valid = sHave && frame - sFrame <= MAX_AGE_FRAMES && sNear > 0.f && sNear <= sFar && llfinite(sFar);
    if (!valid)
    {
        // the splits as without the fit; a later read back tightens the range from there
        sSmoothNear = cpu_near;
        sSmoothFar = cpu_far;
        sSmoothValid = true;
        return false;
    }

    F32 target_near = llclamp(sNear * NEAR_MARGIN, 0.01f, MAX_NEAR);
    F32 target_far = sFar * FAR_MARGIN + FAR_PAD;
    const bool moved = dist_vec(camera.getOrigin(), sOrigin) > MOVE_METRES + 0.05f * sFar ||
                       camera.getAtAxis() * sAt < TURN_COS;
    if (moved)
    {
        // the read back may not show what is in view now
        target_near = cpu_near;
        target_far = cpu_far;
    }
    target_far = llclamp(target_far, target_near + 1.f, cpu_far);
    target_near = llmin(target_near, target_far - 1.f);

    if (!sSmoothValid)
    {
        sSmoothNear = cpu_near;
        sSmoothFar = cpu_far;
        sSmoothValid = true;
    }

    // grow at once, shrink smoothly
    const F32 keep = expf(-dt / SHRINK_SECONDS);
    sSmoothFar = target_far >= sSmoothFar ? target_far : target_far + (sSmoothFar - target_far) * keep;
    sSmoothNear = target_near <= sSmoothNear ? target_near : target_near + (sSmoothNear - target_near) * keep;

    far_out = llmin(quantize_up(sSmoothFar), cpu_far);
    near_out = llclamp(quantize_down(sSmoothNear), 0.01f, llmax(far_out - 1.f, 0.01f));
    if (!(far_out > near_out))
    {
        return false;
    }
    sRangeCut = far_out < cpu_far * 0.98f;
    sCutRatio = llclamp(far_out / cpu_far, 0.f, 1.f);
    return true;
}

bool FSShadowDepthFit::rangeCut()
{
    return sRangeCut;
}

F32 FSShadowDepthFit::fadeLength(F32 fade_length)
{
    if (!sRangeCut)
    {
        return fade_length;
    }
    // The range ends at the farthest depth read back times FAR_MARGIN plus FAR_PAD, so a fade over its
    // last CUT_FADE starts past everything the read back saw, and only what came into view since
    // (a read back a few frames old, a turn too small to count as one) falls in it: a soft end
    // instead of a line across the ground. Towards the uncut range (a cut ratio of 0.8 to 0.98) the
    // fade widens to the one set, so it doesn't jump when the cut comes and goes.
    const F32 t = llclamp((sCutRatio - 0.8f) / 0.18f, 0.f, 1.f);
    return llmin(fade_length, CUT_FADE + (fade_length - CUT_FADE) * t);
}

void FSShadowDepthFit::release()
{
    release_levels();
    release_readback();
    sFirstProgram.unload();
    sReduceProgram.unload();
    sProgramsTried = false;
}
