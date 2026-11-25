/**
 * @file fsgpuocclusion.cpp
 * @brief FSRenderGPUOcclusionCulling: two-phase occlusion culling of the GPU-driven G-buffer draws
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

// How it fits together (see fsgpuocclusion.h for the idea):
//
// Boxes. LLVolumeGeometryManager::registerFace gives a batch the union of its faces' extents, moved into the
// space its vertices are written in: face extents are in that space already for active objects, and in agent
// space (region space plus the region's origin) for static ones, whose vertices are region space. Rigged
// faces, flexible objects, global volumes and animated children (whose vertices are written with another
// transform than their extents) give the batch no box. rebuildMesh rewrites vertices in place without new
// batches, so it takes the boxes of the group's batches away before, and gives them back after
// (refreshGroupBounds): the drawables' updateGeometry brought their faces' extents up to date before the group
// is rebuilt, and a batch is the faces whose indices lie in its range of its vertex buffer, so a batch whose
// indices are all those of faces with a box gets the union again. A batch whose vertex buffer was uploaded again
// since its box was first used (other than by rebuildMesh) loses its box. A batch without a box is drawn every
// frame, as before.
//
// Rigged batches (the rigged passes) get their box each frame instead, from their avatar: the extents the stock
// code culls the avatar with (LLVOAvatar::getLastAnimExtents: joints, static attachments and the rigged meshes'
// joint boxes, refreshed every few frames and moved with the pelvis in between; an animated object's control
// avatar has its own), padded generously, in agent space, where the skinned vertices end up: a rigged batch has
// no model matrix, so its record's modelview is the view alone. The own avatar and the animated objects it wears
// get none (always near the camera, and their extents can lag a fast animation), nor do extents that don't hold
// the avatar's position (not computed yet).
//
// Slots. A batch with a box gets a slot when it is first drawn: its visibility bit in a buffer that lives
// across frames (0 for a new slot: it is tested in phase 2 of its first frame). ~LLDrawInfo frees it.
//
// Per pass (cullPhaseOne): the pass's commands, records, matrix table and material table are copied, on the
// GPU, into buffers of the frame's pass slot (the pass's own buffers get new storage at its next upload, but
// phase 2 needs them once the opaque pools are done), the cull entries are uploaded, and the phase 1 compute
// writes the two command lists. The pass draws the first.
//
// flushPhaseTwo: the farthest depth pyramid (fsOcclusionHiZC.glsl) from the depth buffer of the G-buffer
// target, phase 2 (fsOcclusionCullC.glsl with FS_OCC_PHASE2) for every pass that drew, then their replays.
// Should the target no longer be the one the passes drew to, phase 2 runs without the pyramid, which draws
// every batch in the frustum that phase 1 left out: nothing is lost.
//
// Packing (glMultiDrawElementsIndirectCount, OpenGL 4.6): after each cull, fsOcclusionCompactC.glsl copies the
// commands that draw, in their order, to the front of their multi-draw's range in a second list and writes the
// range's count, which the multi-draw reads as its draw count. A pass whose ranges overlap or leave its commands,
// or a driver without the call or the program, draws the unpacked lists as before.
//
// The programs are compiled here, raw, with "#version 430 core" (the shader manager's loader only knows vertex
// and fragment shaders and puts a 400/420 version on them), the first time a frame wants them, and their
// uniform locations looked up once. Binding them goes around LLGLSLShader, so the bound program is put back
// after each use; texture unit 0 goes through LLTexUnit, so its cache stays right.

#include "llviewerprecompiledheaders.h"

#include "fsgpuocclusion.h"

#include "fsglstream.h"
#include "fsskinpalettebuffer.h"

#include "lldir.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llcontrolavatar.h"
#include "llvoavatar.h"
#include "llvovolume.h"
#include "pipeline.h"

#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>

#ifndef GL_PARAMETER_BUFFER
#define GL_PARAMETER_BUFFER 0x80EE
#endif

extern bool gCubeSnapshot;

namespace
{
    constexpr U32 MAX_SLOTS = 1u << 21;     // 8 MB of visibility; past that new batches go without a slot
    constexpr U32 STAT_STRIDE = 16;         // uints per kind of pass in the statistics buffer (fsOcclusionCullC.glsl uses 0..8)
    constexpr U32 STAT_COUNT = STAT_STRIDE * FSGPUOcclusion::KIND_COUNT;
    constexpr U32 STAT_READ_DELAY = 4;      // frames between a sample and reading it back
    constexpr GLuint TEX_UNIT = 0;          // the depth buffer, then the pyramid, for the compute passes

    //-------------------------------------------------------------------------
    // Slots

    std::mutex sSlotMutex;                  // ~LLDrawInfo may run off the main thread
    std::vector<U32> sFreeSlots;
    U32 sNextSlot = 1;                      // 0: no slot
    std::atomic<bool> sBoundsMade{ false }; // a box was ever made: rebuildMesh has boxes to take away

    U32 allocate_slot()
    {
        std::lock_guard<std::mutex> lock(sSlotMutex);
        if (!sFreeSlots.empty())
        {
            const U32 slot = sFreeSlots.back();
            sFreeSlots.pop_back();
            return slot;
        }
        if (sNextSlot >= MAX_SLOTS)
        {
            return 0;
        }
        return sNextSlot++;
    }

    U32 slot_end()
    {
        std::lock_guard<std::mutex> lock(sSlotMutex);
        return sNextSlot;
    }

    //-------------------------------------------------------------------------
    // Programs

    struct CullProgram
    {
        GLuint mProgram = 0;
        GLint mCount = -1;
        GLint mRecordStride = -1;
        GLint mMatrixField = -1;
        GLint mMVPOffset = -1;
        GLint mCollect = -1;
        GLint mStatsBase = -1;
        GLint mScreen = -1;     // phase 2
        GLint mLevels = -1;
    };

    struct HiZProgram
    {
        GLuint mProgram = 0;
        GLint mSrcSize = -1;
        GLint mDstSize = -1;
    };

    CullProgram sCull[2];       // phase 1, phase 2
    HiZProgram sHiZ[2];         // level 0 from the depth buffer, a level from the one before
    GLuint sCompact = 0;        // packing (optional: 0 without glMultiDrawElementsIndirectCount or the program)
    GLint sCompactBase = -1;
    bool sProgramsTried = false;
    bool sProgramsReady = false;

    std::string read_shader(const std::string& file)
    {
        const std::string& delim = gDirUtilp->getDirDelimiter();
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "shaders",
                                                                "class1" + delim + "deferred" + delim + file);
        llifstream in(path.c_str());
        if (!in.is_open())
        {
            LL_WARNS("ShaderLoading") << "FSRenderGPUOcclusionCulling: can't open " << path << LL_ENDL;
            return std::string();
        }
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    GLuint compile_compute(const std::string& file, const char* defines, const char* name)
    {
        const std::string body = read_shader(file);
        if (body.empty())
        {
            return 0;
        }
        std::string source = "#version 430 core\n";
        source += defines;
        source += body;
        const GLchar* text = source.c_str();

        GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
        if (!shader)
        {
            return 0;
        }
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE)
        {
            GLchar log[2048] = {};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            LL_WARNS("ShaderLoading") << "FSRenderGPUOcclusionCulling: " << name << " failed to compile: " << log << LL_ENDL;
            glDeleteShader(shader);
            return 0;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, shader);
        glLinkProgram(program);
        glDeleteShader(shader);     // goes with the program
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE)
        {
            GLchar log[2048] = {};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            LL_WARNS("ShaderLoading") << "FSRenderGPUOcclusionCulling: " << name << " failed to link: " << log << LL_ENDL;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    void delete_programs()
    {
        for (CullProgram& p : sCull)
        {
            if (p.mProgram)
            {
                glDeleteProgram(p.mProgram);
            }
            p = CullProgram();
        }
        for (HiZProgram& p : sHiZ)
        {
            if (p.mProgram)
            {
                glDeleteProgram(p.mProgram);
            }
            p = HiZProgram();
        }
        if (sCompact)
        {
            glDeleteProgram(sCompact);
        }
        sCompact = 0;
        sCompactBase = -1;
        sProgramsReady = false;
    }

    // Once per GL context (releaseGL starts over)
    bool ensure_programs()
    {
        if (sProgramsTried)
        {
            return sProgramsReady;
        }
        sProgramsTried = true;
        if (!gGLManager.mFSHasGPUDrivenDraws || !glDispatchCompute || !glMemoryBarrier || !glBindImageTexture || !glTexStorage2D)
        {
            LL_INFOS("ShaderLoading") << "FSRenderGPUOcclusionCulling: needs OpenGL 4.3 compute shaders and the GPU-driven "
                                      << "draws; drawing without it" << LL_ENDL;
            return false;
        }
        // one burst of synchronous calls (compile status, uniform locations) instead of a wait each
        FSGLStream::DirectScope direct;

        const char* reverse_z = LLGLSLShader::sGlobalDefines.count("REVERSE_Z") ? "#define REVERSE_Z 1\n" : "";
        std::string phase1 = reverse_z;
        std::string phase2 = std::string(reverse_z) + "#define FS_OCC_PHASE2 1\n";
        std::string from_depth = std::string(reverse_z) + "#define FS_HIZ_FROM_DEPTH 1\n";
        sCull[0].mProgram = compile_compute("fsOcclusionCullC.glsl", phase1.c_str(), "the phase 1 cull");
        sCull[1].mProgram = compile_compute("fsOcclusionCullC.glsl", phase2.c_str(), "the phase 2 cull");
        sHiZ[0].mProgram = compile_compute("fsOcclusionHiZC.glsl", from_depth.c_str(), "the depth pyramid's first level");
        sHiZ[1].mProgram = compile_compute("fsOcclusionHiZC.glsl", reverse_z, "the depth pyramid's levels");
        if (!sCull[0].mProgram || !sCull[1].mProgram || !sHiZ[0].mProgram || !sHiZ[1].mProgram)
        {
            LL_WARNS("ShaderLoading") << "FSRenderGPUOcclusionCulling: a compute program failed to build; drawing without "
                                      << "occlusion culling" << LL_ENDL;
            delete_programs();
            return false;
        }
        for (CullProgram& p : sCull)
        {
            p.mCount = glGetUniformLocation(p.mProgram, "fs_count");
            p.mRecordStride = glGetUniformLocation(p.mProgram, "fs_record_stride");
            p.mMatrixField = glGetUniformLocation(p.mProgram, "fs_matrix_field");
            p.mMVPOffset = glGetUniformLocation(p.mProgram, "fs_mvp_offset");
            p.mCollect = glGetUniformLocation(p.mProgram, "fs_collect");
            p.mStatsBase = glGetUniformLocation(p.mProgram, "fs_stats_base");
            p.mScreen = glGetUniformLocation(p.mProgram, "fs_screen");
            p.mLevels = glGetUniformLocation(p.mProgram, "fs_levels");
        }
        for (HiZProgram& p : sHiZ)
        {
            p.mSrcSize = glGetUniformLocation(p.mProgram, "fs_src_size");
            p.mDstSize = glGetUniformLocation(p.mProgram, "fs_dst_size");
        }
        if (sCull[0].mCount < 0 || sCull[1].mCount < 0 || sCull[1].mScreen < 0 || sCull[1].mLevels < 0 ||
            sHiZ[0].mDstSize < 0 || sHiZ[1].mDstSize < 0)
        {
            LL_WARNS("ShaderLoading") << "FSRenderGPUOcclusionCulling: a compute program lacks its uniforms; drawing without "
                                      << "occlusion culling" << LL_ENDL;
            delete_programs();
            return false;
        }
        // packing, where the draw count can come from a buffer
        if (glMultiDrawElementsIndirectCount)
        {
            sCompact = compile_compute("fsOcclusionCompactC.glsl", "", "the command packing");
            sCompactBase = sCompact ? glGetUniformLocation(sCompact, "fs_count_base") : -1;
            if (sCompact && sCompactBase < 0)
            {
                glDeleteProgram(sCompact);
                sCompact = 0;
            }
        }
        LL_INFOS("ShaderLoading") << "FSRenderGPUOcclusionCulling: compute programs ready; "
                                  << (sCompact ? "packed draws with glMultiDrawElementsIndirectCount"
                                               : "unpacked draws (no glMultiDrawElementsIndirectCount or packing program)")
                                  << LL_ENDL;
        sProgramsReady = true;
        return true;
    }

    // glUseProgram went around LLGLSLShader: put its program back
    void restore_program()
    {
        glUseProgram(LLGLSLShader::sCurBoundShader);
    }

    //-------------------------------------------------------------------------
    // GPU buffers

    struct GPUBuffer
    {
        GLuint mName = 0;
        size_t mCapacity = 0;

        // New storage of at least bytes (the frame before may still read the old), data in front if given
        void respecify(size_t bytes, const void* data = nullptr)
        {
            bytes = llmax(bytes, (size_t)16);
            if (!mName)
            {
                glGenBuffers(1, &mName);
            }
            if (bytes > mCapacity)
            {
                mCapacity = bytes + bytes / 2;
            }
            glBindBuffer(GL_COPY_WRITE_BUFFER, mName);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)mCapacity, nullptr, GL_DYNAMIC_COPY);
            if (data)
            {
                glBufferSubData(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)bytes, data);
            }
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        // New storage, then bytes of src (from offset) at its start
        void copyFrom(GLuint src, size_t offset, size_t bytes)
        {
            respecify(bytes);
            if (!src || !bytes)
            {
                return;
            }
            glBindBuffer(GL_COPY_READ_BUFFER, src);
            glBindBuffer(GL_COPY_WRITE_BUFFER, mName);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)offset, 0, (GLsizeiptr)bytes);
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

        void release()
        {
            if (mName)
            {
                glDeleteBuffers(1, &mName);
            }
            mName = 0;
            mCapacity = 0;
        }
    };

    // A pass that culls in this frame: copies of its inputs, its two command lists
    struct PassSlot
    {
        GPUBuffer mCommands1;
        GPUBuffer mCommands2;
        GPUBuffer mPacked1;     // packing: the commands of each phase that draw, at the front of their range
        GPUBuffer mPacked2;
        GPUBuffer mRanges;      // packing: first and count per range
        GPUBuffer mCounts;      // packing: per range the phase 1 draw count, then per range the phase 2 one
        U32 mRangeCount = 0;    // 0: not packed
        GPUBuffer mEntries;
        GPUBuffer mRecords;
        GPUBuffer mMatrices;
        GPUBuffer mMaterials;
        U32 mCount = 0;
        U32 mRecordStride = 4;
        U32 mMatrixField = 0;
        U32 mMVPOffset = 1;
        U32 mKind = FSGPUOcclusion::KIND_PBR;
        glm::mat4 mProjection;          // gGL's matrices when the pass drew (its replay draws with them)
        glm::mat4 mModelview;
        FSGPUOcclusion::replay_fn mReplay = nullptr;
        U32 mUser = 0;

        void release()
        {
            mCommands1.release();
            mCommands2.release();
            mPacked1.release();
            mPacked2.release();
            mRanges.release();
            mCounts.release();
            mEntries.release();
            mRecords.release();
            mMatrices.release();
            mMaterials.release();
        }
    };

    std::vector<PassSlot> sPasses;
    U32 sPassCount = 0;             // this frame's

    GPUBuffer sVisibility;          // a uint per slot, across frames
    U32 sVisibilitySlots = 0;
    GPUBuffer sStats;               // fsOcclusionCullC.glsl's counters, on sample frames
    GPUBuffer sStatsReadback;

    // the pyramid
    GLuint sPyramid = 0;
    U32 sPyramidWidth = 0;          // level 0 (half the depth buffer, rounded down)
    U32 sPyramidHeight = 0;
    U32 sPyramidLevels = 0;

    // the frame
    bool sFrameActive = false;
    bool sFlushed = false;
    const LLRenderTarget* sTarget = nullptr;    // the target the passes drew to

    // statistics: every 10 s while on, per kind of pass (FSGPUOcclusion::EKind)
    constexpr U32 KINDS = FSGPUOcclusion::KIND_COUNT;
    const char* const KIND_NAMES[KINDS] = { "PBR", "materials", "legacy", "rigged" };
    LLFrameTimer sStatTimer;
    U64 sStatFrames = 0;
    U64 sStatNoPyramid = 0;
    U64 sStatPasses[KINDS] = {};
    U64 sStatCommands[KINDS] = {};
    U64 sStatBoxed[KINDS] = {};
    U64 sStatTriangles[KINDS] = {};
    U64 sFrameTriangles[KINDS] = {};    // this frame's commands
    bool sSamplePending = false;        // a sample was taken, not read back yet
    bool sSampleRequested = false;
    U32 sSampleFrame = 0;
    U64 sSampleTriangles[KINDS] = {};
    std::atomic<bool> sSampleReady{ false };
    U32 sSampleValues[STAT_COUNT] = {};

    // Render thread (or main thread, not recording), in stream order: the sample, copied a few frames ago
    void read_sample(U64 name)
    {
        if (glGetNamedBufferSubData && name)
        {
            glGetNamedBufferSubData((GLuint)name, 0, sizeof(sSampleValues), sSampleValues);
        }
        sSampleReady.store(true, std::memory_order_release);
    }

    bool target_ok(const LLRenderTarget* rt)
    {
        return rt && rt->getDepth() && rt->getUsage() == LLTexUnit::TT_TEXTURE && rt->getWidth() == LLRenderTarget::sCurResX &&
               rt->getHeight() == LLRenderTarget::sCurResY && rt->getWidth() > 1 && rt->getHeight() > 1;
    }

    void release_pyramid()
    {
        if (sPyramid)
        {
            for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; ++i)
            {
                LLTexUnit* unit = gGL.getTexUnit(i);
                if (unit && unit->getCurrTexture() == sPyramid)
                {
                    unit->unbind(LLTexUnit::TT_TEXTURE);
                }
            }
            glDeleteTextures(1, &sPyramid);
        }
        sPyramid = 0;
        sPyramidWidth = sPyramidHeight = sPyramidLevels = 0;
    }

    bool allocate_pyramid(U32 width, U32 height)
    {
        const U32 w = llmax(width >> 1, 1u);
        const U32 h = llmax(height >> 1, 1u);
        if (sPyramid && w == sPyramidWidth && h == sPyramidHeight)
        {
            return true;
        }
        release_pyramid();
        U32 levels = 1;
        while ((llmax(w, h) >> levels) > 0)
        {
            ++levels;
        }
        glGenTextures(1, &sPyramid);
        if (!sPyramid)
        {
            return false;
        }
        LLTexUnit* unit = gGL.getTexUnit(TEX_UNIT);
        unit->bindManual(LLTexUnit::TT_TEXTURE, sPyramid, true);
        glTexStorage2D(GL_TEXTURE_2D, (GLsizei)levels, GL_R32F, (GLsizei)w, (GLsizei)h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        unit->unbind(LLTexUnit::TT_TEXTURE);
        sPyramidWidth = w;
        sPyramidHeight = h;
        sPyramidLevels = levels;
        return true;
    }

    // The farthest depth pyramid of rt's depth buffer
    bool build_pyramid(LLRenderTarget& rt)
    {
        const U32 width = rt.getWidth();
        const U32 height = rt.getHeight();
        if (!allocate_pyramid(width, height))
        {
            return false;
        }
        LL_PROFILE_GPU_ZONE("occlusion hi-z");
        gGL.flush();
        LLTexUnit* unit = gGL.getTexUnit(TEX_UNIT);
        const U32 sampler = unit->getCurrSampler();

        // level 0 from the depth buffer: point sampling, no comparison, whatever the unit had
        glUseProgram(sHiZ[0].mProgram);
        unit->bind(&rt, true);
        unit->bindSampler(gGL.getDepthFetchSampler());
        glUniform2i(sHiZ[0].mSrcSize, (GLint)width, (GLint)height);
        glUniform2i(sHiZ[0].mDstSize, (GLint)sPyramidWidth, (GLint)sPyramidHeight);
        glBindImageTexture(0, sPyramid, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glDispatchCompute((sPyramidWidth + 7) / 8, (sPyramidHeight + 7) / 8, 1);

        // each further level from the one before, through image units: the pyramid is never sampled meanwhile
        glUseProgram(sHiZ[1].mProgram);
        U32 src_w = sPyramidWidth;
        U32 src_h = sPyramidHeight;
        for (U32 level = 1; level < sPyramidLevels; ++level)
        {
            const U32 dst_w = llmax(src_w >> 1, 1u);
            const U32 dst_h = llmax(src_h >> 1, 1u);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            glBindImageTexture(1, sPyramid, (GLint)(level - 1), GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            glBindImageTexture(0, sPyramid, (GLint)level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            glUniform2i(sHiZ[1].mSrcSize, (GLint)src_w, (GLint)src_h);
            glUniform2i(sHiZ[1].mDstSize, (GLint)dst_w, (GLint)dst_h);
            glDispatchCompute((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
            src_w = dst_w;
            src_h = dst_h;
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        unit->unbind(LLTexUnit::TT_TEXTURE);
        unit->bindSampler(sampler);
        return true;
    }

    // The visibility buffer holds slots [0, end): grown keeping what it had, new slots 0 (tested in phase 2)
    void ensure_visibility(U32 end)
    {
        if (sVisibility.mName && end <= sVisibilitySlots)
        {
            return;
        }
        U32 slots = llmax(sVisibilitySlots * 2, 16384u);
        while (slots < end)
        {
            slots *= 2;
        }
        slots = llmin(slots, MAX_SLOTS);
        GPUBuffer grown;
        const std::vector<U32> zeros(slots, 0u);
        grown.respecify(slots * sizeof(U32), zeros.data());
        if (sVisibility.mName && sVisibilitySlots)
        {
            glBindBuffer(GL_COPY_READ_BUFFER, sVisibility.mName);
            glBindBuffer(GL_COPY_WRITE_BUFFER, grown.mName);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)(sVisibilitySlots * sizeof(U32)));
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }
        sVisibility.release();
        sVisibility = grown;
        sVisibilitySlots = slots;
    }

    void bind_cull_buffers(const PassSlot& pass)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pass.mCommands1.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, pass.mCommands2.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pass.mEntries.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pass.mRecords.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, pass.mMatrices.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, sVisibility.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, sStats.mName);
    }

    void unbind_cull_buffers()
    {
        for (GLuint i = 0; i < 7; ++i)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, 0);
        }
    }

    void set_cull_uniforms(const CullProgram& program, const PassSlot& pass, bool collect)
    {
        glUniform1ui(program.mCount, pass.mCount);
        glUniform1ui(program.mRecordStride, pass.mRecordStride);
        glUniform1ui(program.mMatrixField, pass.mMatrixField);
        glUniform1ui(program.mMVPOffset, pass.mMVPOffset);
        if (program.mCollect >= 0)
        {
            glUniform1ui(program.mCollect, collect ? 1u : 0u);
        }
        if (program.mStatsBase >= 0)
        {
            glUniform1ui(program.mStatsBase, pass.mKind * STAT_STRIDE);
        }
    }

    // Packs the commands of src that draw into dst (the pass's ranges); counts at the range's place from base
    void pack(const PassSlot& pass, const GPUBuffer& src, const GPUBuffer& dst, U32 base)
    {
        glUniform1ui(sCompactBase, base);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, src.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dst.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, pass.mRanges.mName);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, pass.mCounts.mName);
        glDispatchCompute(pass.mRangeCount, 1, 1);
    }

    // Whether ranges can be packed: within the commands, none empty, none overlapping
    bool ranges_ok(const FSGPUOcclusion::Range* ranges, U32 range_count, U32 command_count)
    {
        if (!ranges || !range_count)
        {
            return false;
        }
        static std::vector<U8> used;
        used.assign(command_count, 0);
        for (U32 r = 0; r < range_count; ++r)
        {
            const FSGPUOcclusion::Range& range = ranges[r];
            if (!range.mCount || range.mFirst >= command_count || range.mCount > command_count - range.mFirst)
            {
                return false;
            }
            for (U32 c = range.mFirst; c < range.mFirst + range.mCount; ++c)
            {
                if (used[c])
                {
                    return false;
                }
                used[c] = 1;
            }
        }
        return true;
    }

    void log_stats(U32 frame)
    {
        if (sSamplePending && !sSampleRequested && frame - sSampleFrame >= STAT_READ_DELAY)
        {
            FSGLStream::recordCall(&read_sample, (U64)sStatsReadback.mName);
            sSampleRequested = true;
        }
        if (sStatTimer.getElapsedTimeF32() < 10.f || (sSamplePending && !sSampleReady.load(std::memory_order_acquire)))
        {
            return;
        }
        std::ostringstream out;
        out << "GPU occlusion culling, last 10 s: " << sStatFrames << " frames, " << sStatNoPyramid
            << " without a pyramid (all in view drawn)";
        for (U32 k = 0; k < KINDS; ++k)
        {
            if (!sStatPasses[k])
            {
                continue;
            }
            out << "; " << KIND_NAMES[k] << ": " << sStatPasses[k] << " passes culled, " << sStatCommands[k] << " commands ("
                << sStatBoxed[k] << " with a box), " << sStatTriangles[k] << " triangles";
            if (sSamplePending && glGetNamedBufferSubData)
            {
                const U32* v = sSampleValues + k * STAT_STRIDE;
                const U64 culled_tris = (U64)v[5] + (U64)v[6];
                out << "; one frame: " << v[0] << " batches tested, " << v[1] << " drawn in phase 1 (last frame's visible; "
                    << v[8] << " of them hidden by the end), " << v[2] << " drawn in phase 2 (newly visible), " << v[3]
                    << " culled outside the frustum (" << v[5] << " triangles), " << v[4] << " culled as hidden (" << v[6]
                    << " triangles), " << v[7] << " without a box; " << culled_tris << " of " << sSampleTriangles[k]
                    << " triangles culled (" << (sSampleTriangles[k] ? (U32)(culled_tris * 100 / sSampleTriangles[k]) : 0)
                    << "%)";
            }
        }
        out << "; pyramid " << sPyramidWidth << "x" << sPyramidHeight << ", " << sPyramidLevels << " levels; "
            << slot_end() - 1 << " slots";
        LL_INFOS("FSGPUOcclusion") << out.str() << LL_ENDL;
        sStatTimer.reset();
        sStatFrames = sStatNoPyramid = 0;
        for (U32 k = 0; k < KINDS; ++k)
        {
            sStatPasses[k] = sStatCommands[k] = sStatBoxed[k] = sStatTriangles[k] = 0;
        }
        sSamplePending = false;
        sSampleRequested = false;
        sSampleReady.store(false, std::memory_order_relaxed);
    }
}

//-----------------------------------------------------------------------------
// Boxes and slots

namespace
{
    // facep's box in the space its batch's vertices are in; false when it has no reliable one
    bool face_box(LLFace* facep, LLVector4a box[2])
    {
        if (!facep || facep->mAvatar || facep->mSkinInfo.notNull())
        {
            return false;
        }
        LLDrawable* drawable = facep->getDrawable();
        if (!drawable || drawable->isDead() || drawable->isState(LLDrawable::RIGGED | LLDrawable::ANIMATED_CHILD))
        {
            return false;
        }
        LLVOVolume* vobj = drawable->getVOVolume();
        if (!vobj || vobj->isFlexible() || vobj->isVolumeGlobal() || vobj->isRiggedMesh())
        {
            return false;
        }
        // FSRenderKeepSelfLoaded: your own attachments are never culled by the test, like your rigged meshes
        // (a batch made while the setting was off keeps its box until its group is rebuilt)
        static LLCachedControl<bool> keep_self_loaded(gSavedSettings, "FSRenderKeepSelfLoaded", true);
        if (keep_self_loaded && LLVOAvatar::fsIsSelfPart(vobj))
        {
            return false;
        }
        box[0] = facep->mExtents[0];
        box[1] = facep->mExtents[1];
        if (!drawable->isActive())
        {   // genVolumeBBoxes moved them into agent space; the vertices are in region space
            LLViewerRegion* region = drawable->getRegion();
            if (!region)
            {
                return false;
            }
            LLVector4a offset;
            offset.load3(region->getOriginAgent().mV);
            box[0].sub(offset);
            box[1].sub(offset);
        }
        // a box that isn't one (never generated, or garbage) is no box
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 lo = box[0][i];
            const F32 hi = box[1][i];
            if (!llfinite(lo) || !llfinite(hi) || lo > hi || hi - lo > 8192.f || fabsf(lo) > 1.0e6f || fabsf(hi) > 1.0e6f)
            {
                return false;
            }
        }
        return true;
    }
}

namespace
{
    // A rigged batch's box in agent space (see the top of the file); false when it has no reliable one
    bool rigged_box(const LLDrawInfo& info, LLVector4a box[2])
    {
        LLVOAvatar* avatar = info.mAvatar.get();
        if (!avatar || info.mModelMatrix || avatar->isDead() || avatar->isSelf() || avatar->mDrawable.isNull())
        {
            return false;
        }
        if (avatar->isControlAvatar())
        {   // an animated object worn by the own avatar is the own avatar's
            LLVOVolume* root = static_cast<LLControlAvatar*>(avatar)->mRootVolp;
            if (!root || root->isDead())
            {
                return false;
            }
            if (root->isAttachment())
            {
                LLVOAvatar* wearer = root->getAvatarAncestor();
                if (!wearer || wearer->isSelf())
                {
                    return false;
                }
            }
        }
        const LLVector3* ext = avatar->getLastAnimExtents();
        if (!ext || ext[0] == LLVector3::zero || ext[1] == LLVector3::zero)
        {   // not computed yet (the stock code takes zero for that too)
            return false;
        }
        const LLVector3 pos = avatar->getRenderPosition();
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 lo = ext[0].mV[i];
            const F32 hi = ext[1].mV[i];
            if (!llfinite(lo) || !llfinite(hi) || lo > hi || hi - lo > 1024.f || fabsf(lo) > 1.0e6f || fabsf(hi) > 1.0e6f)
            {
                return false;
            }
            // generously: the extents are refreshed only every few frames, and an animation moves the limbs and
            // rigged meshes in between
            const F32 pad = 1.f + (hi - lo) * 0.25f;
            box[0].getF32ptr()[i] = lo - pad;
            box[1].getF32ptr()[i] = hi + pad;
            if (!llfinite(pos.mV[i]) || pos.mV[i] < lo - pad || pos.mV[i] > hi + pad)
            {   // extents that don't hold the avatar are stale
                return false;
            }
        }
        return true;
    }
}

bool FSGPUOcclusion::enabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderGPUOcclusionCulling", false);
    return enabled;
}

void FSGPUOcclusion::initBatchBounds(LLDrawInfo& info, LLFace* facep)
{
    info.mFSHasBounds = false;
    if (!enabled())
    {
        return;
    }
    LLVector4a box[2];
    if (face_box(facep, box))
    {
        info.mFSBounds[0] = box[0];
        info.mFSBounds[1] = box[1];
        info.mFSHasBounds = true;
        sBoundsMade.store(true, std::memory_order_relaxed);
    }
}

void FSGPUOcclusion::extendBatchBounds(LLDrawInfo& info, LLFace* facep)
{
    if (!info.mFSHasBounds)
    {
        return;
    }
    LLVector4a box[2];
    if (!face_box(facep, box))
    {   // one face without a box: the batch has none
        info.mFSHasBounds = false;
        return;
    }
    info.mFSBounds[0].setMin(info.mFSBounds[0], box[0]);
    info.mFSBounds[1].setMax(info.mFSBounds[1], box[1]);
}

void FSGPUOcclusion::invalidateGroupBounds(LLSpatialGroup* group)
{
    if (!group || !sBoundsMade.load(std::memory_order_relaxed))
    {
        return;
    }
    for (auto& pass : group->mDrawMap)
    {
        for (LLPointer<LLDrawInfo>& info : pass.second)
        {
            if (info.notNull())
            {
                info->mFSHasBounds = false;
            }
        }
    }
}

void FSGPUOcclusion::refreshGroupBounds(LLSpatialGroup* group)
{
    if (!group || !enabled() || !sBoundsMade.load(std::memory_order_relaxed))
    {
        return;
    }
    // the group's static batches, by vertex buffer and first index (main thread, like rebuildMesh)
    struct Batch
    {
        LLDrawInfo* mInfo;
        const LLVertexBuffer* mBuffer;
        LLVector4a mBox[2];
        U32 mCovered;   // indices of faces found in it
        bool mBad;      // a face without a box
    };
    static std::vector<Batch> batches;
    batches.clear();
    for (auto& pass : group->mDrawMap)
    {
        for (LLPointer<LLDrawInfo>& info : pass.second)
        {
            if (info.notNull() && info->mVertexBuffer.notNull() && info->mCount && info->mAvatar.isNull() &&
                info->mSkinInfo.isNull())
            {
                Batch batch;
                batch.mInfo = info.get();
                batch.mBuffer = info->mVertexBuffer.get();
                batch.mCovered = 0;
                batch.mBad = false;
                batches.push_back(batch);
            }
        }
    }
    if (batches.empty())
    {
        return;
    }
    auto before = [](const Batch& a, const Batch& b)
    {
        return a.mBuffer != b.mBuffer ? std::less<const LLVertexBuffer*>()(a.mBuffer, b.mBuffer) : a.mInfo->mOffset < b.mInfo->mOffset;
    };
    std::sort(batches.begin(), batches.end(), before);

    for (LLSpatialGroup::element_iter it = group->getDataBegin(); it != group->getDataEnd(); ++it)
    {
        LLDrawable* drawable = (LLDrawable*)(*it)->getDrawable();
        if (!drawable || drawable->isDead())
        {
            continue;
        }
        for (S32 i = 0; i < drawable->getNumFaces(); ++i)
        {
            LLFace* face = drawable->getFace(i);
            const LLVertexBuffer* vb = face ? face->getVertexBuffer() : nullptr;
            const U32 count = face ? face->getIndicesCount() : 0;
            if (!vb || !count)
            {
                continue;
            }
            const U32 start = (U32)face->getIndicesStart();
            LLVector4a box[2];
            const bool ok = face_box(face, box);
            // every pass's batch of this buffer whose range holds the face's indices
            auto b = std::lower_bound(batches.begin(), batches.end(), vb,
                                      [](const Batch& batch, const LLVertexBuffer* buffer)
                                      { return std::less<const LLVertexBuffer*>()(batch.mBuffer, buffer); });
            for (; b != batches.end() && b->mBuffer == vb && b->mInfo->mOffset <= start; ++b)
            {
                if (start + count > b->mInfo->mOffset + b->mInfo->mCount)
                {
                    continue;
                }
                if (!ok)
                {
                    b->mBad = true;
                }
                else if (!b->mCovered)
                {
                    b->mBox[0] = box[0];
                    b->mBox[1] = box[1];
                }
                else
                {
                    b->mBox[0].setMin(b->mBox[0], box[0]);
                    b->mBox[1].setMax(b->mBox[1], box[1]);
                }
                b->mCovered += count;
            }
        }
    }

    // a batch whose indices are all those of faces with a box; its vertex buffer's serial is taken at its next use
    for (const Batch& batch : batches)
    {
        if (!batch.mBad && batch.mCovered == batch.mInfo->mCount)
        {
            batch.mInfo->mFSBounds[0] = batch.mBox[0];
            batch.mInfo->mFSBounds[1] = batch.mBox[1];
            batch.mInfo->mFSBoundsSerial = 0;
            batch.mInfo->mFSHasBounds = true;
        }
    }
}

void FSGPUOcclusion::onSettingChanged(bool on)
{
    if (!on || !gPipeline.isInit())
    {
        return;
    }
    // Boxes are made when batches are built; the batches already there have none. Every spatial group with a
    // volume is rebuilt once (as after a teleport), so they get theirs.
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* objectp = gObjectList.getObject(i);
        if (!objectp || objectp->isDead() || objectp->getPCode() != LL_PCODE_VOLUME)
        {
            continue;
        }
        LLDrawable* drawable = objectp->mDrawable;
        LLSpatialGroup* group = drawable && !drawable->isDead() ? drawable->getSpatialGroup() : nullptr;
        if (group && !group->hasState(LLSpatialGroup::GEOM_DIRTY))
        {
            group->dirtyGeom();
            gPipeline.markRebuild(group);
        }
    }
}

void FSGPUOcclusion::releaseSlot(U32 slot)
{
    if (!slot)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sSlotMutex);
    sFreeSlots.push_back(slot);
}

void FSGPUOcclusion::makeEntry(LLDrawInfo& info, CullEntry& entry)
{
    entry.mSlot = 0;
    entry.mTriangles = info.mCount / 3;
    if (info.mAvatar.notNull() || info.mSkinInfo.notNull())
    {   // rigged: a box from its avatar, made now
        LLVector4a box[2];
        if (!rigged_box(info, box))
        {
            entry.mMin[0] = entry.mMin[1] = entry.mMin[2] = 0.f;
            entry.mMax[0] = entry.mMax[1] = entry.mMax[2] = 0.f;
            return;
        }
        if (!info.mFSOcclusionSlot)
        {
            info.mFSOcclusionSlot = allocate_slot();
        }
        entry.mSlot = info.mFSOcclusionSlot;
        for (S32 i = 0; i < 3; ++i)
        {
            entry.mMin[i] = box[0][i];
            entry.mMax[i] = box[1][i];
        }
        return;
    }
    if (info.mFSHasBounds && info.mVertexBuffer.notNull())
    {   // the box holds the vertices as they were when it was first used; uploaded again since: no box
        const U32 serial = info.mVertexBuffer->getFSContentSerial();
        if (!info.mFSBoundsSerial)
        {
            info.mFSBoundsSerial = serial;
        }
        else if (info.mFSBoundsSerial != serial)
        {
            info.mFSHasBounds = false;
        }
    }
    if (!info.mFSHasBounds)
    {
        entry.mMin[0] = entry.mMin[1] = entry.mMin[2] = 0.f;
        entry.mMax[0] = entry.mMax[1] = entry.mMax[2] = 0.f;
        return;
    }
    if (!info.mFSOcclusionSlot)
    {
        info.mFSOcclusionSlot = allocate_slot();
    }
    entry.mSlot = info.mFSOcclusionSlot;
    // a little bigger: float rounding between the face extents and the vertices, and polygon offset
    for (S32 i = 0; i < 3; ++i)
    {
        const F32 lo = info.mFSBounds[0][i];
        const F32 hi = info.mFSBounds[1][i];
        const F32 pad = 0.01f + (hi - lo) * 0.01f;
        entry.mMin[i] = lo - pad;
        entry.mMax[i] = hi + pad;
    }
}

//-----------------------------------------------------------------------------
// The frame

void FSGPUOcclusion::beginFrame(bool for_snapshot)
{
    sFrameActive = false;
    sFlushed = false;
    sPassCount = 0;
    sTarget = nullptr;
    for (U64& triangles : sFrameTriangles)
    {
        triangles = 0;
    }
    if (!enabled() || for_snapshot || !gGLManager.mFSHasGPUDrivenDraws)
    {
        return;
    }
    sFrameActive = ensure_programs();
    if (sFrameActive)
    {
        ++sStatFrames;
    }
}

void FSGPUOcclusion::endFrame()
{
    // renderGeomDeferred always reaches flushPhaseTwo; should it ever not, the batches phase 1 left out are
    // drawn here rather than lost
    flushPhaseTwo();
    sFrameActive = false;
}

bool FSGPUOcclusion::passActive()
{
    if (!sFrameActive || sFlushed || gCubeSnapshot || LLPipeline::sShadowRender || LLPipeline::sReflectionRender ||
        LLPipeline::sRenderingHUDs || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return false;
    }
    const LLRenderTarget* rt = LLRenderTarget::getCurrentBoundTarget();
    if (!target_ok(rt) || (sTarget && sTarget != rt))
    {
        return false;
    }
    return true;
}

S32 FSGPUOcclusion::cullPhaseOne(const PassInputs& inputs, const std::vector<CullEntry>& entries)
{
    if (!passActive() || !inputs.mCommandCount || entries.size() != inputs.mCommandCount || !inputs.mCommands ||
        !inputs.mRecords || !inputs.mMatrices)
    {
        return -1;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LL_PROFILE_GPU_ZONE("occlusion cull 1");
    sTarget = LLRenderTarget::getCurrentBoundTarget();

    if (sPassCount >= sPasses.size())
    {
        sPasses.resize(sPassCount + 1);
    }
    const S32 index = (S32)sPassCount++;
    PassSlot& pass = sPasses[index];
    pass.mCount = inputs.mCommandCount;
    pass.mRecordStride = inputs.mRecordStride;
    pass.mMatrixField = inputs.mMatrixField;
    pass.mMVPOffset = inputs.mMVPOffset;
    pass.mKind = inputs.mKind < KINDS ? inputs.mKind : (U32)KIND_PBR;
    pass.mProjection = gGL.getProjectionMatrix();
    pass.mModelview = gGL.getModelviewMatrix();
    pass.mReplay = nullptr;
    pass.mUser = 0;

    const size_t command_bytes = (size_t)inputs.mCommandCount * 5 * sizeof(U32);
    pass.mCommands2.copyFrom(inputs.mCommands, inputs.mCommandsOffset, command_bytes);
    pass.mCommands1.respecify(command_bytes);
    pass.mEntries.respecify(entries.size() * sizeof(CullEntry), entries.data());
    pass.mRangeCount = 0;
    if (sCompact && ranges_ok(inputs.mRanges, inputs.mRangeCount, inputs.mCommandCount))
    {
        pass.mRangeCount = inputs.mRangeCount;
        pass.mPacked1.respecify(command_bytes);
        pass.mPacked2.respecify(command_bytes);
        pass.mRanges.respecify(pass.mRangeCount * sizeof(Range), inputs.mRanges);
        pass.mCounts.respecify(pass.mRangeCount * 2 * sizeof(U32));
    }
    pass.mRecords.copyFrom(inputs.mRecords, inputs.mRecordsOffset, inputs.mRecordBytes);
    pass.mMatrices.copyFrom(inputs.mMatrices, inputs.mMatricesOffset, inputs.mMatrixBytes);
    pass.mMaterials.copyFrom(inputs.mMaterials, inputs.mMaterialsOffset, inputs.mMaterialBytes);
    ensure_visibility(slot_end());
    if (!sStats.mName)
    {
        const U32 zeros[STAT_COUNT] = {};
        sStats.respecify(sizeof(zeros), zeros);
        sStatsReadback.respecify(sizeof(zeros), zeros);
    }

    glUseProgram(sCull[0].mProgram);
    set_cull_uniforms(sCull[0], pass, false);
    bind_cull_buffers(pass);
    glDispatchCompute((pass.mCount + 63) / 64, 1, 1);
    glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    unbind_cull_buffers();
    if (pass.mRangeCount)
    {   // the phase 1 commands that draw, packed
        glUseProgram(sCompact);
        pack(pass, pass.mCommands1, pass.mPacked1, 0);
        glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
        unbind_cull_buffers();
    }
    restore_program();

    const U32 kind = pass.mKind;
    ++sStatPasses[kind];
    sStatCommands[kind] += pass.mCount;
    for (const CullEntry& entry : entries)
    {
        sStatBoxed[kind] += entry.mSlot ? 1 : 0;
        sFrameTriangles[kind] += entry.mTriangles;
    }
    return index;
}

GLuint FSGPUOcclusion::phaseOneCommands(S32 pass)
{
    if (pass < 0 || (U32)pass >= sPassCount)
    {
        return 0;
    }
    const PassSlot& slot = sPasses[pass];
    return slot.mRangeCount ? slot.mPacked1.mName : slot.mCommands1.mName;
}

void FSGPUOcclusion::drawRange(S32 pass, bool phase2, U32 range, U32 first, U32 count)
{
    const void* commands = (const void*)((size_t)first * 5 * sizeof(U32));
    if (pass >= 0 && (U32)pass < sPassCount && sPasses[pass].mRangeCount && range < sPasses[pass].mRangeCount)
    {
        const PassSlot& slot = sPasses[pass];
        const GLintptr drawcount = (GLintptr)(((phase2 ? slot.mRangeCount : 0) + range) * sizeof(U32));
        glBindBuffer(GL_PARAMETER_BUFFER, slot.mCounts.mName);
        glMultiDrawElementsIndirectCount(GL_TRIANGLES, GL_UNSIGNED_SHORT, commands, drawcount, (GLsizei)count, 0);
        glBindBuffer(GL_PARAMETER_BUFFER, 0);
        return;
    }
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, commands, (GLsizei)count, 0);
}

void FSGPUOcclusion::addReplay(S32 pass, replay_fn replay, U32 user)
{
    if (pass >= 0 && (U32)pass < sPassCount)
    {
        sPasses[pass].mReplay = replay;
        sPasses[pass].mUser = user;
    }
}

GLuint FSGPUOcclusion::replayCommands(S32 pass)
{
    if (pass < 0 || (U32)pass >= sPassCount)
    {
        return 0;
    }
    const PassSlot& slot = sPasses[pass];
    return slot.mRangeCount ? slot.mPacked2.mName : slot.mCommands2.mName;
}

GLuint FSGPUOcclusion::replayRecords(S32 pass)
{
    return pass >= 0 && (U32)pass < sPassCount ? sPasses[pass].mRecords.mName : 0;
}

GLuint FSGPUOcclusion::replayMatrices(S32 pass)
{
    return pass >= 0 && (U32)pass < sPassCount ? sPasses[pass].mMatrices.mName : 0;
}

GLuint FSGPUOcclusion::replayMaterials(S32 pass)
{
    return pass >= 0 && (U32)pass < sPassCount ? sPasses[pass].mMaterials.mName : 0;
}

bool FSGPUOcclusion::refreshPalettes(S32 pass, const std::vector<LLDrawInfo*>& params, std::vector<U32>& records, U32 stride,
                                     U32 field, U32& generation)
{
    if (pass < 0 || (U32)pass >= sPassCount || !stride || field >= stride || records.size() != params.size() * stride ||
        !sPasses[pass].mRecords.mName || records.size() * sizeof(U32) > sPasses[pass].mRecords.mCapacity)
    {
        return false;
    }
    static std::vector<U32> offsets;
    static const std::vector<LLDrawInfo*> none;
    if (!FSSkinPaletteBuffer::place(params, none, offsets, generation) || offsets.size() != params.size())
    {
        return false;
    }
    for (size_t c = 0; c < params.size(); ++c)
    {
        // the top bit is no part of the offset: skin SSS's exclusion (FS_GDD_SKIN_EXCLUDE_BIT), kept
        records[c * stride + field] = (records[c * stride + field] & FS_GDD_SKIN_EXCLUDE_BIT) | offsets[c];
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, sPasses[pass].mRecords.mName);
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)(records.size() * sizeof(U32)), records.data());
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    return true;
}

void FSGPUOcclusion::flushPhaseTwo()
{
    if (!sFrameActive || sFlushed)
    {
        return;
    }
    sFlushed = true;
    const U32 frame = LLFrameTimer::getFrameCount();
    bool any = false;
    for (U32 i = 0; i < sPassCount; ++i)
    {
        any = any || sPasses[i].mReplay;
    }
    if (any && LLRenderTarget::getCurrentBoundTarget() != sTarget)
    {   // Not expected (renderGeomDeferred calls this with the G-buffer still bound): the replays would draw
        // G-buffer batches into whatever is bound. Left out instead; the batches phase 1 left out are missing
        // this frame, and their visibility stays as it was, so the next frame tests them again.
        LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: the G-buffer was no longer bound for phase 2; skipped" << LL_ENDL;
        any = false;
    }
    if (any)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gpu occlusion phase 2");
        LL_PROFILE_GPU_ZONE("occlusion phase 2");
        LLGLSLShader* bound = LLGLSLShader::sCurBoundShaderPtr;
        LLVertexBuffer::unbind();

        // the pyramid, from the target the passes drew to if it is still the one bound
        LLRenderTarget* rt = LLRenderTarget::getCurrentBoundTarget();
        const bool pyramid = rt && rt == sTarget && target_ok(rt) && build_pyramid(*rt);
        if (!pyramid)
        {
            ++sStatNoPyramid;
        }

        // a sample for the statistics line: counted this frame, read back a few frames later
        const bool collect = !sSamplePending && sStatTimer.getElapsedTimeF32() >= 10.f && glGetNamedBufferSubData;
        if (collect)
        {
            const U32 zeros[STAT_COUNT] = {};
            sStats.respecify(sizeof(zeros), zeros);
        }

        {
            LL_PROFILE_GPU_ZONE("occlusion cull 2");
            LLTexUnit* unit = gGL.getTexUnit(TEX_UNIT);
            const U32 sampler = unit->getCurrSampler();
            const CullProgram& program = sCull[1];
            glUseProgram(program.mProgram);
            if (pyramid)
            {
                unit->bindManual(LLTexUnit::TT_TEXTURE, sPyramid, true);
                unit->bindSampler(0);   // the texture's own (mipmapped, point) filtering
            }
            glUniform2i(program.mScreen, rt ? (GLint)rt->getWidth() : 1, rt ? (GLint)rt->getHeight() : 1);
            glUniform1i(program.mLevels, pyramid ? (GLint)sPyramidLevels : 0);
            for (U32 i = 0; i < sPassCount; ++i)
            {
                const PassSlot& pass = sPasses[i];
                if (!pass.mReplay)
                {
                    continue;
                }
                set_cull_uniforms(program, pass, collect);
                bind_cull_buffers(pass);
                glDispatchCompute((pass.mCount + 63) / 64, 1, 1);
            }
            glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            unbind_cull_buffers();
            // the phase 2 commands that draw, packed
            bool packed = false;
            for (U32 i = 0; i < sPassCount; ++i)
            {
                const PassSlot& pass = sPasses[i];
                if (pass.mReplay && pass.mRangeCount)
                {
                    if (!packed)
                    {
                        glUseProgram(sCompact);
                        packed = true;
                    }
                    pack(pass, pass.mCommands2, pass.mPacked2, pass.mRangeCount);
                }
            }
            if (packed)
            {
                glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
                unbind_cull_buffers();
            }
            if (pyramid)
            {
                unit->unbind(LLTexUnit::TT_TEXTURE);
                unit->bindSampler(sampler);
            }
            restore_program();
        }

        if (collect)
        {
            glBindBuffer(GL_COPY_READ_BUFFER, sStats.mName);
            glBindBuffer(GL_COPY_WRITE_BUFFER, sStatsReadback.mName);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, STAT_COUNT * sizeof(U32));
            glBindBuffer(GL_COPY_READ_BUFFER, 0);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            sSamplePending = true;
            sSampleRequested = false;
            sSampleReady.store(false, std::memory_order_relaxed);
            sSampleFrame = frame;
            for (U32 k = 0; k < KINDS; ++k)
            {
                sSampleTriangles[k] = sFrameTriangles[k];
            }
        }

        // the batches phase 1 left out that are visible after all, each pass with its own programs and with the
        // matrices it drew with (the rigged programs project with gGL's)
        const LLRender::eMatrixMode mode = gGL.getMatrixMode();
        for (U32 i = 0; i < sPassCount; ++i)
        {
            const PassSlot& pass = sPasses[i];
            if (pass.mReplay)
            {
                gGL.matrixMode(LLRender::MM_PROJECTION);
                gGL.pushMatrix();
                gGL.loadMatrix(glm::value_ptr(pass.mProjection));
                gGL.matrixMode(LLRender::MM_MODELVIEW);
                gGL.pushMatrix();
                gGL.loadMatrix(glm::value_ptr(pass.mModelview));
                pass.mReplay((S32)i, pass.mUser);
                gGL.matrixMode(LLRender::MM_PROJECTION);
                gGL.popMatrix();
                gGL.matrixMode(LLRender::MM_MODELVIEW);
                gGL.popMatrix();
            }
        }
        gGL.matrixMode(mode);

        if (bound)
        {
            bound->bind();
        }
        else
        {
            LLGLSLShader::unbind();
        }
        LLVertexBuffer::unbind();
    }
    for (U32 k = 0; k < KINDS; ++k)
    {
        sStatTriangles[k] += sFrameTriangles[k];
    }
    log_stats(frame);
}

void FSGPUOcclusion::releaseGL()
{
    for (PassSlot& pass : sPasses)
    {
        pass.release();
    }
    sPasses.clear();
    sPassCount = 0;
    sVisibility.release();
    sVisibilitySlots = 0;
    sStats.release();
    sStatsReadback.release();
    sSamplePending = false;
    sSampleRequested = false;
    release_pyramid();
    delete_programs();
    sProgramsTried = false;     // built again for the next context
    sFrameActive = false;
}
