/**
 * @file llrendertarget.cpp
 * @brief LLRenderTarget implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llrendertarget.h"
#include "llrender.h"
#include "llgl.h"
#include "fsglstream.h" // <FS:Trish/> FSRenderThread
#include "fsreversez.h" // <FS:Trish/> FSRenderReverseZ
#include <atomic> // <FS:Trish/>

LLRenderTarget* LLRenderTarget::sBoundTarget = NULL;
U32 LLRenderTarget::sBytesAllocated = 0;

void check_framebuffer_status()
{
    if (gDebugGL)
    {
        GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        switch (status)
        {
        case GL_FRAMEBUFFER_COMPLETE:
            break;
        default:
            LL_WARNS() << "check_framebuffer_status failed -- " << std::hex << status << LL_ENDL;
            ll_fail("check_framebuffer_status failed");
            break;
        }
    }
}

// <FS:Trish> FSRenderThread: glGetError makes the main thread wait for the render thread to work
// through everything queued, once per attachment (impostors as avatars switch, resizes, dynamic
// resolution). While it records, the check runs on the render thread in stream order and only logs.
// Once one such check has found a failure, video memory is short and more are likely: from then on
// the check waits for its answer, so the callers see the failure and take their fallbacks.
// A failure found after the fact marks its target (LLRenderTarget::mFSAllocFailed): isComplete()
// is false from then on and allocate() makes it again, instead of the owner drawing into and
// sampling a texture without storage.
static std::atomic<bool> sFSAllocFailedSeen{ false };

// What the render thread checks: the target's flag (shared, so it outlives the target) and which buffer
struct FSAllocCheck
{
    std::shared_ptr<std::atomic<bool>> mFailed;
    bool mDepth;
};

// Render thread, in stream order. Owns and frees the check.
static void fs_check_alloc_error(U64 value)
{
    FSAllocCheck* check = reinterpret_cast<FSAllocCheck*>(value);
    if (glGetError() != GL_NO_ERROR)
    {
        LL_WARNS() << (check->mDepth ? "Unable to allocate depth buffer for render target."
                                     : "Could not allocate color buffer for render target.")
                   << " Found after the fact (render thread): that target is incomplete until it is allocated again;"
                   << " later allocations are checked at once." << LL_ENDL;
        if (check->mFailed)
        {
            check->mFailed->store(true, std::memory_order_release);
        }
        sFSAllocFailedSeen.store(true, std::memory_order_relaxed);
    }
    delete check;
}

// True when the allocation just made failed (while recording, false until a failure has been seen
// once; a failure found later sets flag)
static bool fs_alloc_failed(bool depth, const std::shared_ptr<std::atomic<bool>>& flag)
{
    if (FSGLStream::isRecording() && !sFSAllocFailedSeen.load(std::memory_order_relaxed))
    {
        FSGLStream::recordCall(&fs_check_alloc_error, reinterpret_cast<U64>(new FSAllocCheck{ flag, depth }));
        return false;
    }
    return glGetError() != GL_NO_ERROR;
}

// This allocation's flag, made with its first buffer
const std::shared_ptr<std::atomic<bool>>& LLRenderTarget::fsAllocFlag()
{
    if (!mFSAllocFailed)
    {
        mFSAllocFailed = std::make_shared<std::atomic<bool>>(false);
    }
    return mFSAllocFailed;
}
// </FS:Trish>

bool LLRenderTarget::sUseFBO = false;
U32 LLRenderTarget::sCurFBO = 0;


extern S32 gGLViewport[4];

U32 LLRenderTarget::sCurResX = 0;
U32 LLRenderTarget::sCurResY = 0;

LLRenderTarget::LLRenderTarget() :
    mResX(0),
    mResY(0),
    mFBO(0),
    mDepth(0),
    mUseDepth(false),
    mUsage(LLTexUnit::TT_TEXTURE)
{
}

LLRenderTarget::~LLRenderTarget()
{
    release();
}

void LLRenderTarget::resize(U32 resx, U32 resy)
{
    //for accounting, get the number of pixels added/subtracted
    S32 pix_diff = (resx*resy)-(mResX*mResY);

    mResX = resx;
    mResY = resy;

    llassert(mInternalFormat.size() == mTex.size());

    for (U32 i = 0; i < mTex.size(); ++i)
    { //resize color attachments
        gGL.getTexUnit(0)->bindManual(mUsage, mTex[i]);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, mInternalFormat[i], mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        sBytesAllocated += pix_diff*4;
    }

    if (mDepth)
    {
        gGL.getTexUnit(0)->bindManual(mUsage, mDepth);
        U32 internal_type = LLTexUnit::getInternalType(mUsage);
        // <FS:Trish> FSRenderReverseZ: the format it was made with
        //LLImageGL::setManualImage(internal_type, 0, GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL, false);
        const U32 depth_format = mFSDepthFormat ? mFSDepthFormat : (U32)GL_DEPTH_COMPONENT24;
        LLImageGL::setManualImage(internal_type, 0, depth_format, mResX, mResY, GL_DEPTH_COMPONENT,
                                  depth_format == GL_DEPTH_COMPONENT32F ? GL_FLOAT : GL_UNSIGNED_INT, NULL, false);
        // </FS:Trish>

        sBytesAllocated += pix_diff*4;
    }
}


bool LLRenderTarget::allocate(U32 resx, U32 resy, U32 color_fmt, bool depth, LLTexUnit::eTextureType usage, LLTexUnit::eTextureMipGeneration generateMipMaps)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(usage == LLTexUnit::TT_TEXTURE);
    llassert(!isBoundInStack());

    // <FS:Trish> FSRenderReverseZ: and a depth buffer of the format the depth convention now wants
    //if(mResX == resx && mResY == resy && mUsage == usage && depth == mUseDepth && mGenerateMipMaps == generateMipMaps)
    // and again when the render thread found the last allocation failed (FSRenderThread)
    if(mResX == resx && mResY == resy && mUsage == usage && depth == mUseDepth && mGenerateMipMaps == generateMipMaps &&
       (!depth || !mDepth || mFSDepthFormat == (U32)FSReverseZ::depthFormat()) && !fsAllocFailed())
    // </FS:Trish>
    {
        return true;
    }
    resx = llmin(resx, (U32) gGLManager.mGLMaxTextureSize);
    resy = llmin(resy, (U32) gGLManager.mGLMaxTextureSize);

    release();

    mResX = resx;
    mResY = resy;

    mUsage = usage;
    mUseDepth = depth;

    mGenerateMipMaps = generateMipMaps;

    if (mGenerateMipMaps != LLTexUnit::TMG_NONE) {
        // Calculate the number of mip levels based upon resolution that we should have.
        mMipLevels = 1 + (U32)floor(log10((float)llmax(mResX, mResY)) / log10(2.0));
    }

    if (depth)
    {
        if (!allocateDepth())
        {
            LL_WARNS() << "Failed to allocate depth buffer for render target." << LL_ENDL;
            return false;
        }
    }

    glGenFramebuffers(1, (GLuint *) &mFBO);

    if (mDepth)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFBO);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), mDepth, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    }

    return addColorAttachment(color_fmt);
}

void LLRenderTarget::setColorAttachment(LLImageGL* img, LLGLuint use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(img != nullptr); // img must not be null
    llassert(sUseFBO); // FBO support must be enabled
    llassert(mDepth == 0); // depth buffers not supported with this mode
    llassert(mTex.empty()); // mTex must be empty with this mode (binding target should be done via LLImageGL)
    llassert(!isBoundInStack());

    if (mFBO == 0)
    {
        glGenFramebuffers(1, (GLuint*)&mFBO);
    }

    mResX = img->getWidth();
    mResY = img->getHeight();
    mUsage = img->getTarget();

    if (use_name == 0)
    {
        use_name = img->getTexName();
    }

    mTex.push_back(use_name);

    glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            LLTexUnit::getInternalType(mUsage), use_name, 0);
        stop_glerror();

    check_framebuffer_status();

    glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
}

void LLRenderTarget::releaseColorAttachment()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(!isBoundInStack());
    llassert(mTex.size() == 1); //cannot use releaseColorAttachment with LLRenderTarget managed color targets
    llassert(mFBO != 0);  // mFBO must be valid

    glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, LLTexUnit::getInternalType(mUsage), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);

    mTex.clear();
}

bool LLRenderTarget::addColorAttachment(U32 color_fmt)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    if (color_fmt == 0)
    {
        return true;
    }

    U32 offset = static_cast<U32>(mTex.size());

    if( offset >= 4 )
    {
        LL_WARNS() << "Too many color attachments" << LL_ENDL;
        llassert( offset < 4 );
        return false;
    }
    if( offset > 0 && (mFBO == 0) )
    {
        llassert(  mFBO != 0 );
        return false;
    }

    U32 tex;
    LLImageGL::generateTextures(1, &tex, LLTexUnit::getInternalType(mUsage));
    LLTexUnit::registerFilterCacheTexture(tex); // <FS:Trish/> let bindTexture skip redundant filter changes
    gGL.getTexUnit(0)->bindManual(mUsage, tex);

    stop_glerror();


    {
        clear_glerror();
        LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, color_fmt, mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        // <FS:Trish> FSRenderThread
        //if (glGetError() != GL_NO_ERROR)
        if (fs_alloc_failed(false, fsAllocFlag()))
        // </FS:Trish>
        {
            LL_WARNS() << "Could not allocate color buffer for render target." << LL_ENDL;
            return false;
        }
    }

    sBytesAllocated += mResX*mResY*4;

    stop_glerror();


    if (offset == 0)
    { //use bilinear filtering on single texture render targets that aren't multisampled
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        stop_glerror();
    }
    else
    { //don't filter data attachments
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        stop_glerror();
    }

    if (mUsage != LLTexUnit::TT_RECT_TEXTURE)
    {
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_MIRROR);
        stop_glerror();
    }
    else
    {
        // ATI doesn't support mirrored repeat for rectangular textures.
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        stop_glerror();
    }

    if (mFBO)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0+offset,
            LLTexUnit::getInternalType(mUsage), tex, 0);

        check_framebuffer_status();

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    }

    mTex.push_back(tex);
    mInternalFormat.push_back(color_fmt);

    if (gDebugGL)
    { //bind and unbind to validate target
        bindTarget();
        flush();
    }


    return true;
}

bool LLRenderTarget::allocateDepth()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLImageGL::generateTextures(1, &mDepth, LLTexUnit::getInternalType(mUsage));
    gGL.getTexUnit(0)->bindManual(mUsage, mDepth);

    U32 internal_type = LLTexUnit::getInternalType(mUsage);
    stop_glerror();
    clear_glerror();
    // <FS:Trish> FSRenderReverseZ: float depth under reverse-Z (where its precision is spread evenly
    // in distance), 24 bit fixed point otherwise and for the shadow maps
    //LLImageGL::setManualImage(internal_type, 0, GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL, false);
    mFSDepthFormat = (U32)FSReverseZ::depthFormat();
    LLImageGL::setManualImage(internal_type, 0, mFSDepthFormat, mResX, mResY, GL_DEPTH_COMPONENT,
                              mFSDepthFormat == GL_DEPTH_COMPONENT32F ? GL_FLOAT : GL_UNSIGNED_INT, NULL, false);
    // </FS:Trish>
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

    sBytesAllocated += mResX*mResY*4;

    // <FS:Trish> FSRenderThread
    //if (glGetError() != GL_NO_ERROR)
    if (fs_alloc_failed(true, fsAllocFlag()))
    // </FS:Trish>
    {
        LL_WARNS() << "Unable to allocate depth buffer for render target." << LL_ENDL;
        return false;
    }

    return true;
}

void LLRenderTarget::shareDepthBuffer(LLRenderTarget& target)
{
    llassert(!isBoundInStack());

    if (!mFBO || !target.mFBO)
    {
        LL_ERRS() << "Cannot share depth buffer between non FBO render targets." << LL_ENDL;
    }

    if (target.mDepth)
    {
        LL_ERRS() << "Attempting to override existing depth buffer.  Detach existing buffer first." << LL_ENDL;
    }

    if (target.mUseDepth)
    {
        LL_ERRS() << "Attempting to override existing shared depth buffer. Detach existing buffer first." << LL_ENDL;
    }

    if (mDepth)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, target.mFBO);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), mDepth, 0);

        check_framebuffer_status();

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);

        target.mUseDepth = true;
    }
}

void LLRenderTarget::release()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    if (mDepth)
    {
        LLImageGL::deleteTextures(1, &mDepth);

        mDepth = 0;
        mFSDepthFormat = 0; // <FS:Trish/> FSRenderReverseZ

        sBytesAllocated -= mResX*mResY*4;
    }
    // else if (mFBO)
    if (mFBO)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFBO);

        if (mUseDepth)
        { //detach shared depth buffer
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), 0, 0);
            mUseDepth = false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    }

    // Detach any extra color buffers (e.g. SRGB spec buffers)
    //
    if (mFBO && (mTex.size() > 1))
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
        size_t z;
        for (z = mTex.size() - 1; z >= 1; z--)
        {
            sBytesAllocated -= mResX*mResY*4;
            glFramebufferTexture2D(GL_FRAMEBUFFER, static_cast<GLenum>(GL_COLOR_ATTACHMENT0+z), LLTexUnit::getInternalType(mUsage), 0, 0);
            LLTexUnit::unregisterFilterCacheTexture(mTex[z]); // <FS:Trish/>
            LLImageGL::deleteTextures(1, &mTex[z]);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    }

    if (mFBO)
    {
        if (mFBO == sCurFBO)
        {
            sCurFBO = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        glDeleteFramebuffers(1, (GLuint *) &mFBO);
        mFBO = 0;
    }

    if (mTex.size() > 0)
    {
        sBytesAllocated -= mResX*mResY*4;
        LLTexUnit::unregisterFilterCacheTexture(mTex[0]); // <FS:Trish/>
        LLImageGL::deleteTextures(1, &mTex[0]);
    }

    mTex.clear();
    mInternalFormat.clear();
    mFSAllocFailed.reset(); // <FS:Trish/> the next allocation gets a flag of its own

    mResX = mResY = 0;
}

void LLRenderTarget::bindTarget()
{
    // <FS:Trish> Tracy only, too frequent for FSRenderGPUTimings
    //LL_PROFILE_GPU_ZONE("bindTarget");
    LL_PROFILE_GPU_ZONE_FINE("bindTarget");
    // </FS:Trish>
    llassert(mFBO);
    llassert(!isBoundInStack());

    glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
    sCurFBO = mFBO;

    //setup multiple render targets
    GLenum drawbuffers[] = {GL_COLOR_ATTACHMENT0,
                            GL_COLOR_ATTACHMENT1,
                            GL_COLOR_ATTACHMENT2,
                            GL_COLOR_ATTACHMENT3};

    if (mTex.empty())
    { //no color buffer to draw to
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    else
    {
        glDrawBuffers(static_cast<GLsizei>(mTex.size()), drawbuffers);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    check_framebuffer_status();

    glViewport(0, 0, mResX, mResY);
    sCurResX = mResX;
    sCurResY = mResY;

    mPreviousRT = sBoundTarget;
    sBoundTarget = this;
}

void LLRenderTarget::clear(U32 mask_in)
{
    // <FS:Trish> Tracy only, too frequent for FSRenderGPUTimings
    //LL_PROFILE_GPU_ZONE("clear");
    LL_PROFILE_GPU_ZONE_FINE("clear");
    // </FS:Trish>
    llassert(mFBO);
    U32 mask = GL_COLOR_BUFFER_BIT;
    if (mUseDepth)
    {
        mask |= GL_DEPTH_BUFFER_BIT;

    }
    if (mFBO)
    {
        check_framebuffer_status();
        stop_glerror();
        glClear(mask & mask_in);
        stop_glerror();
    }
    else
    {
        LLGLEnable scissor(GL_SCISSOR_TEST);
        glScissor(0, 0, mResX, mResY);
        stop_glerror();
        glClear(mask & mask_in);
    }
}

U32 LLRenderTarget::getTexture(U32 attachment) const
{
    if (attachment >= mTex.size())
    {
        LL_WARNS() << "Invalid attachment index " << attachment << " for size " << mTex.size() << LL_ENDL;
        llassert(false);
        return 0;
    }
    return mTex[attachment];
}

U32 LLRenderTarget::getNumTextures() const
{
    return static_cast<U32>(mTex.size());
}

void LLRenderTarget::bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options)
{
    gGL.getTexUnit(channel)->bindManual(mUsage, getTexture(index), filter_options == LLTexUnit::TFO_TRILINEAR || filter_options == LLTexUnit::TFO_ANISOTROPIC);
    // <FS:Trish> Skip the glTexParameter calls when the texture already has this filtering
    //gGL.getTexUnit(channel)->setTextureFilteringOption(filter_options);
    LLTexUnit* unit = gGL.getTexUnit(channel);
    if (!unit->isFilteringOptionCached(filter_options))
    {
        unit->activate(); // the unit may not be active when bindManual had nothing to bind
        unit->setTextureFilteringOption(filter_options);
    }
    // </FS:Trish>
}

void LLRenderTarget::flush()
{
    // <FS:Trish> Tracy only, too frequent for FSRenderGPUTimings
    //LL_PROFILE_GPU_ZONE("rt flush");
    LL_PROFILE_GPU_ZONE_FINE("rt flush");
    // </FS:Trish>
    gGL.flush();
    llassert(mFBO);
    llassert(sCurFBO == mFBO);
    llassert(sBoundTarget == this);

    if (mGenerateMipMaps == LLTexUnit::TMG_AUTO)
    {
        LL_PROFILE_GPU_ZONE("rt generate mipmaps");
        bindTexture(0, 0, LLTexUnit::TFO_TRILINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    if (mPreviousRT)
    {
        // a bit hacky -- pop the RT stack back two frames and push
        // the previous frame back on to play nice with the GL state machine
        sBoundTarget = mPreviousRT->mPreviousRT;
        mPreviousRT->bindTarget();
    }
    else
    {
        sBoundTarget = nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        sCurFBO = 0;
        glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        sCurResX = gGLViewport[2];
        sCurResY = gGLViewport[3];
        glReadBuffer(GL_BACK);
        glDrawBuffer(GL_BACK);
    }
}

void LLRenderTarget::copyContents(LLRenderTarget& source, S32 srcX0, S32 srcY0, S32 srcX1, S32 srcY1, S32 dstX0, S32 dstY0, S32 dstX1,
                                  S32 dstY1, U32 mask, U32 filter)
{
    // <FS:Trish> Tracy only, too frequent for FSRenderGPUTimings
    //LL_PROFILE_GPU_ZONE("LLRenderTarget::copyContents");
    LL_PROFILE_GPU_ZONE_FINE("LLRenderTarget::copyContents");
    // </FS:Trish>

    GLboolean write_depth = mask & GL_DEPTH_BUFFER_BIT ? GL_TRUE : GL_FALSE;

    LLGLDepthTest depth(write_depth, write_depth);

    gGL.flush();
    if (!source.mFBO || !mFBO)
    {
        LL_WARNS() << "Cannot copy framebuffer contents for non FBO render targets." << LL_ENDL;
        return;
    }

    // Firestorm's render targets all allocate depth as
    // GL_DEPTH_COMPONENT24, so the newer Alchemy depth/stencil mismatch
    // fallback is unnecessary here.
    // <FS:Trish> FSRenderReverseZ: or all as GL_DEPTH_COMPONENT32F, but the shadow maps and their
    // caches, which only copy among themselves. A depth blit needs both formats alike.
    // </FS:Trish>
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source.mFBO);
    stop_glerror();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFBO);
    stop_glerror();
    check_framebuffer_status();
    stop_glerror();
    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1,
        dstX0, dstY0, dstX1, dstY1, mask, filter);
    stop_glerror();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    stop_glerror();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    stop_glerror();
    glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    stop_glerror();
}

// static
void LLRenderTarget::copyContentsToFramebuffer(LLRenderTarget& source, S32 srcX0, S32 srcY0, S32 srcX1, S32 srcY1, S32 dstX0, S32 dstY0,
                                               S32 dstX1, S32 dstY1, U32 mask, U32 filter)
{
    if (!source.mFBO)
    {
        LL_WARNS() << "Cannot copy framebuffer contents for non FBO render targets." << LL_ENDL;
        return;
    }

    {
        // <FS:Trish> Tracy only, too frequent for FSRenderGPUTimings
        //LL_PROFILE_GPU_ZONE("copyContentsToFramebuffer");
        LL_PROFILE_GPU_ZONE_FINE("copyContentsToFramebuffer");
        // </FS:Trish>
        GLboolean write_depth = mask & GL_DEPTH_BUFFER_BIT ? GL_TRUE : GL_FALSE;

        LLGLDepthTest depth(write_depth, write_depth);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, source.mFBO);
        stop_glerror();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        stop_glerror();
        check_framebuffer_status();
        stop_glerror();
        glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        stop_glerror();
        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
        stop_glerror();
    }
}

bool LLRenderTarget::isComplete() const
{
    // <FS:Trish> Not when the render thread found its allocation failed (FSRenderThread)
    //return !mTex.empty() || mDepth;
    return (!mTex.empty() || mDepth) && !fsAllocFailed();
    // </FS:Trish>
}

void LLRenderTarget::getViewport(S32* viewport)
{
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = mResX;
    viewport[3] = mResY;
}

bool LLRenderTarget::isBoundInStack() const
{
    LLRenderTarget* cur = sBoundTarget;
    while (cur && cur != this)
    {
        cur = cur->mPreviousRT;
    }

    return cur == this;
}

void LLRenderTarget::swapFBORefs(LLRenderTarget& other)
{
    // Must be initialized
    llassert(mFBO);
    llassert(other.mFBO);

    // Must be unbound
    // *NOTE: mPreviousRT can be non-null even if this target is unbound - presumably for debugging purposes?
    llassert(sCurFBO != mFBO);
    llassert(sCurFBO != other.mFBO);
    llassert(!isBoundInStack());
    llassert(!other.isBoundInStack());

    // Must be same type
    llassert(sUseFBO == other.sUseFBO);
    llassert(mResX == other.mResX);
    llassert(mResY == other.mResY);
    llassert(mInternalFormat == other.mInternalFormat);
    llassert(mTex.size() == other.mTex.size());
    llassert(mDepth == other.mDepth);
    llassert(mUseDepth == other.mUseDepth);
    llassert(mGenerateMipMaps == other.mGenerateMipMaps);
    llassert(mMipLevels == other.mMipLevels);
    llassert(mUsage == other.mUsage);

    std::swap(mFBO, other.mFBO);
    std::swap(mTex, other.mTex);
    std::swap(mFSAllocFailed, other.mFSAllocFailed); // <FS:Trish/> with the textures it marks
}
