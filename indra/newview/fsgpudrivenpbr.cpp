/**
 * @file fsgpudrivenpbr.cpp
 * @brief GPU-driven draws for the deferred GLTF PBR opaque pass (FSPerfGPUDrivenPBR)
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

// The stock deferred PBR opaque pass (LLRenderPass::pushGLTFBatches) costs, for every batch, a vertex buffer
// bind and attribute setup, a model matrix (modelview, modelview projection and normal matrix uniforms), a
// texture matrix when animated, a cull face toggle for double sided materials, a glDrawRangeElements, and on
// every material change four texture binds and a dozen uniforms. genDrawInfo gives every batch its own
// vertex buffer, so nothing there can be merged.
//
// With FSPerfGPUDrivenPBR on, the batches of the pass come out of a vertex arena instead (fsgddarena.h: a GPU
// side copy of the position, normal, tangent, texcoord0 and colour regions and the 16 bit indices of their
// vertex buffers, refreshed when a buffer's content serial changes), and each batch is one 20 byte indirect
// draw command plus a record: which model matrices (in a storage buffer, the exact matrices the stock path
// would have computed, computed the same way), which texture matrix (identity or the animation's), and which
// material (the KHR texture transforms, alpha cutoff, roughness, metallic and emissive factors bind() would
// have set, and with FSPerfGPUDrivenPBRBindless the four texture handles). pbropaqueV.glsl and pbropaqueF.glsl
// built with FS_GDD read those in place of their uniforms; everything else in them is the stock code.
//
// The batches are drawn in the stock order, as runs: one glMultiDrawElementsIndirect per run of batches that
// share the face culling state and, without bindless textures, the material (whose textures are bound the
// stock way, LLFetchedGLTFMaterial::bind). With bindless textures a run only ends where a batch is double
// sided and the previous one isn't (or the other way round), or a batch that can't use handles comes. A batch
// that can't come from the arena at all (no material, rigged, empty, 32 bit indices, buffer mapped or lacking
// an attribute, arena full or the copy budget used up) is drawn by the stock code in its place, so the order,
// and with it the result where two surfaces have the same depth, is the stock one.
//
// FSPerfGPUDrivenPBRGroupRuns gives that order up: in a busy scene the batches without handles (media, textures
// waiting for their options), the stock fallbacks and the double sided ones are spread through the pass and
// cut it into thousands of runs, each a multi-draw, a program switch or four texture binds. Grouped, all
// bindless batches go out as one multi-draw per face culling state, the bound ones as one run per material,
// and the stock fallbacks after them. The depth test decides what a G-buffer pass leaves, so only where two
// surfaces have exactly the same depth can a different one win than in the stock order.
//
// Bindless textures: a texture with a handle is immutable (no glTexImage, no glTexParameter). The handles are
// made with glGetTextureSamplerHandleARB and a sampler object of our own per texture, set up as LLTexUnit
// would set up the texture (filter, mipmaps, wrap, anisotropy). Before LLImageGL or LLTexUnit respecify a
// texture or change its parameters they call LLImageGL::sFSTextureUnlock, which makes its handles non
// resident and deletes its samplers, which deletes the handles and makes the texture mutable again (checked
// once per GL context: if a driver keeps the texture immutable, bindless is not used). Handles are looked up
// by the texture's current GL name every pass, so a reallocated texture (a new name) gets a new handle. Making
// a handle needs the GL thread, so while FSRenderThread records, a missing handle is queued (the batch takes
// bound textures this time, unless the image's previous texture still has a handle: see previous_handle), the
// queued ones are made together on the render thread at the next frame's first lookup without waiting for it,
// and filed at a later frame's first lookup once made. Only fetched textures (any channel count) that are
// complete, uncompressed, not waiting for their options to be applied, and not media get handles; a batch with
// any other texture is drawn with bound textures. Why lookups found no handle is in the 10 s stats line.
//
// FSPerfGPUDrivenPBRVerify: after the pass, the batches it drew from the arena are drawn again the stock way
// with colour and depth writes off and depth test GL_LESS under a GL_SAMPLES_PASSED query: a sample passes
// only where the stock geometry is nearer than what the multi-draws left, which is logged. It also reads back
// a couple of arena entries per pass and compares them with their vertex buffers, checks for GL errors, and
// logs statistics every 10 seconds (FSPerfGPUDrivenPBRStats logs only the statistics).
//
// FSPerfGPUDrivenRigged (with FSPerfRiggedPaletteBuffer, fsskinpalettebuffer.h): the rigged opaque passes
// (PASS_GLTF_PBR_RIGGED, PASS_GLTF_PBR_ALPHA_MASK_RIGGED: LLRenderPass::pushRiggedGLTFBatches with the skinned
// variant of gDeferredPBROpaqueProgram bound) the same way, both with bound textures and bindless, as in the
// legacy material passes (fsgpudrivenmaterials.cpp). The skinned programs are built from the stock skinned
// one with FS_GDD (and FS_GDD_BINDLESS) and LLShaderFeatures::mFSGDDSkinning, which links the copy of
// objectSkinV.glsl built with FS_GDD_RIGGED. Their batches come from a second arena that also holds the joint
// weights, and each record names (its w, Record::mSkin) where the batch's palette starts in this frame's
// palette buffer, which the pass binds whole as a storage buffer: the palette the stock loop would bind, the
// same floats the stock programs read through FSSkinPalette, read by the same skinning code. The skinned
// branch of pbropaqueV.glsl projects with the projection_matrix uniform, which only gGL.syncMatrices sets: it
// runs before the multi-draws. A batch without avatar, skin or palette yet is skipped, as the stock loop skips
// it. All palettes of the pass go into the buffer before it draws (FSSkinPaletteBuffer::place), the stock
// fallbacks' too; should the buffer still be replaced mid pass, the multi-draws left are drawn the stock way.
//
// FSRenderGPUOcclusionCulling (fsgpuocclusion.h), the main camera's passes only: the pass's commands go through
// the phase 1 cull first, and the multi-draws read the culled copy (sIndirectOverride), which keeps only the
// batches visible last frame. The pass's ops are kept, and once every opaque pool has drawn, replay_phase_two
// draws them again with the phase 2 commands (the batches found visible only then) from copies of the pass's
// records, matrices and materials (sReplayPass). Stock ops are drawn in phase 1 only, as before. A rigged pass
// keeps its batches and records too: should the palette buffer be replaced before phase 2, their palettes are
// put in it again and the records' offsets rewritten (FSGPUOcclusion::refreshPalettes).

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenpbr.h"
#include "fsreversez.h"

#include "fsglretire.h"
#include "fsglstream.h"

#include "fsgddarena.h"
#include "fsgddcommon.h"
#include "fsgpudrivenmaterials.h"
#include "fsgpuocclusion.h"
#include "fsskinpalettebuffer.h"

#include "lldrawpool.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llthread.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <sstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#if defined(GL_ARB_bindless_texture) && !LL_DARWIN
#define FS_GDD_BINDLESS_API 1
#else
#define FS_GDD_BINDLESS_API 0
#endif

LLGLSLShader gFSGDDPBRProgram;
LLGLSLShader gFSGDDPBRBindlessProgram;

namespace
{
    constexpr GLuint FS_GDD_PBR_MATRIX_BINDING = 5;     // shader storage binding of FSGDDPBRMatrices
    constexpr GLuint FS_GDD_PBR_MATERIAL_BINDING = 6;   // and of FSGDDPBRMaterials
    constexpr GLuint FS_GDD_PBR_SKIN_BINDING = 7;       // and of FSGDDSkinPalettes (objectSkinV.glsl, rigged)
    constexpr U32 FS_GDD_PBR_RIGGED = 2;                // program index bit of the rigged programs (bit 0: bindless)
    constexpr U32 FS_GDD_PBR_PROGRAMS = 4;
    constexpr U32 FS_GDD_PBR_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TANGENT |
                                       LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;
    constexpr U32 FS_GDD_HANDLE_IDLE_FRAMES = 300;      // frames a handle may go unused before it is made non resident
    constexpr U32 FS_GDD_HANDLE_DROP_FRAMES = 1800;     // and before it is deleted
    constexpr U32 FS_GDD_HANDLE_RETRY_FRAMES = 60;      // frames before a texture/sampler pair that got no handle is tried again

    // everything pbropaqueV.glsl reads, an eighth of FSPerfGPUDrivenPBRArenaMB for indices
    FSGDDArena sArena("FSPerfGPUDrivenPBR arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TANGENT,
                        LLVertexBuffer::TYPE_TEXCOORD0, LLVertexBuffer::TYPE_COLOR },
                      "FSPerfGPUDrivenPBRArenaMB", 512, 8);
    // FSPerfGPUDrivenRigged: the same plus the joint weights, for the rigged passes, so rigged buffers neither
    // crowd the static ones out nor make every vertex of the first arena carry weights. The same size limit, its own.
    FSGDDArena sRiggedArena("FSPerfGPUDrivenRigged PBR arena",
                            { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TANGENT,
                              LLVertexBuffer::TYPE_TEXCOORD0, LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_WEIGHT4 },
                            "FSPerfGPUDrivenPBRArenaMB", 512, 8);

    struct Record       // fs_gdd_draw
    {
        U32 mMatrix;    // modelview; modelview projection and normal matrix follow
        U32 mTexMatrix;
        U32 mMaterial;
        U32 mSkin;      // rigged: the palette's first vec4 in the palette buffer (FSSkinPaletteBuffer::place)
    };

    struct Material     // FSGDDMaterial, std430
    {
        F32 mTransform[8][4];
        F32 mFactors[4];
        F32 mEmissive[4];
        U32 mTex[8];
    };
    static_assert(sizeof(Material) == 192, "FSGDDMaterial layout");

    enum EOpKind : U8
    {
        OP_BINDLESS = 0,    // gFSGDDPBRBindlessProgram
        OP_BOUND,           // gFSGDDPBRProgram, the material's textures bound
        OP_STOCK            // gDeferredPBROpaqueProgram, LLRenderPass::pushGLTFBatch
    };

    struct Op
    {
        EOpKind mKind;
        bool mDoubleSided;
        U32 mFirst;                     // first command
        U32 mCount;                     // commands
        LLFetchedGLTFMaterial* mMat;    // OP_BOUND
        LLViewerTexture* mTex;          // OP_BOUND: media override
        LLDrawInfo* mParams;            // OP_STOCK
    };

    // why a batch went the stock way, or (bindless on) with bound textures
    enum EReason
    {
        R_NO_MATERIAL = 0,
        R_RIGGED,
        R_EMPTY,
        R_RANGE,
        R_FADING,                       // fading in (FSRenderFadeIn)
        R_ARENA,                        // + FSGDDArena::EFail
        R_BOUND_MEDIA = R_ARENA + FSGDDArena::FAIL_COUNT,
        R_BOUND_TEXTURE,
        R_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "no material", "rigged", "empty", "index range", "fading in" };
        if (reason < R_ARENA)
        {
            return names[reason];
        }
        if (reason < R_BOUND_MEDIA)
        {
            return FSGDDArena::failName(reason - R_ARENA);
        }
        return reason == R_BOUND_MEDIA ? "media (bound textures)" : "texture without handle (bound textures)";
    }

    FSGDDBuffer sIndirectBuffer;
    FSGDDBuffer sRecordBuffer;
    FSGDDBuffer sMaterialBuffer;
    GLuint sQuery = 0;
    GLint sRecordAttrib[FS_GDD_PBR_PROGRAMS] = { -1, -1, -1, -1 };  // location of fs_gdd_draw, by program()
    S32 sInputsReady[FS_GDD_PBR_PROGRAMS] = {};                     // bind_inputs done: 0 not yet, 1 yes, -1 failed
    LLGLSLShader sRiggedProgram;            // FSPerfGPUDrivenRigged: the skinned gFSGDDPBRProgram
    LLGLSLShader sRiggedBindlessProgram;    // and gFSGDDPBRBindlessProgram
    // the pass being drawn: rigged, its stock program (gDeferredPBROpaqueProgram or its skinned variant), its arena
    bool sRiggedPass = false;
    LLGLSLShader* sStockProgram = &gDeferredPBROpaqueProgram;
    FSGDDArena* sPassArena = &sArena;
    // rigged pass: the batches drawn the stock way (their palettes go into the buffer too), the palette
    // offsets' generation (FSSkinPaletteBuffer)
    std::vector<LLDrawInfo*> sStockRigged;
    std::vector<U32> sPaletteOffsets;
    U32 sPaletteGeneration = 0;

    std::vector<Op> sOps;
    std::vector<FSGDDDrawCommand> sCommands;
    std::vector<Record> sRecords;
    FSGDDMatrixTable sMatrices;
    std::vector<Material> sMaterials;
    std::vector<LLDrawInfo*> sCommandParams; // per command: its batch (verify, and the stock fallback)

    // FSRenderGPUOcclusionCulling: the indirect buffer the multi-draws read instead of sIndirectBuffer (at offset
    // 0; 0 for none), the occlusion pass whose input copies setup_inputs binds (phase 2; -1 for none), the
    // cull entries, and per frame what each culled pass's phase 2 needs
    GLuint sIndirectOverride = 0;
    S32 sReplayPass = -1;
    std::vector<FSGPUOcclusion::CullEntry> sCullEntries;
    S32 sCullPass = -1;     // phase 1: the occlusion index of the pass being drawn (-1 for none)
    std::vector<FSGPUOcclusion::Range> sRanges;    // its multi-draw ops' commands, in order
    struct Replay
    {
        std::vector<Op> mOps;               // the multi-draw ops (no stock ops)
        bool mRigged = false;
        // rigged: the palette offsets' generation, the batches (one per command) and a copy of the records
        U32 mGeneration = 0;
        std::vector<LLDrawInfo*> mParams;
        std::vector<U32> mRecords;
    };
    std::vector<Replay> sReplays;
    // FSRenderGPUOcclusionCulling: the multi-draw ops (by range) of the pass being drawn that phase 1 drew the stock
    // way after all (the palette buffer replaced, a program failed to bind): its phase 2 leaves them out, or it
    // drew their batches a second time
    std::vector<U8> sStockRanges;

    void mark_stock(U32 range)
    {
        if (sStockRanges.size() <= range)
        {
            sStockRanges.resize(range + 1, 0);
        }
        sStockRanges[range] = 1;
    }
    U32 sReplayFrame = 0;
    U32 sReplayUsed = 0;    // sReplays of this frame
    struct MaterialInfo
    {
        U32 mIndex;
        bool mBindless;
    };
    U32 sMaterialPass = 1;  // the pass being built; materials remember their record by it (mFSGDDPass)

    // stats, every 10 seconds with FSPerfGPUDrivenPBRVerify or FSPerfGPUDrivenPBRStats
    U64 sStatBatches = 0;
    U64 sStatBindless = 0;
    U64 sStatBound = 0;
    U64 sStatCalls = 0;
    U64 sStatSwitches = 0;
    U64 sStatPasses = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;

    // which: bit 0 bindless, bit 1 rigged (FS_GDD_PBR_RIGGED)
    LLGLSLShader& program(U32 which)
    {
        static LLGLSLShader* const programs[FS_GDD_PBR_PROGRAMS] = { &gFSGDDPBRProgram, &gFSGDDPBRBindlessProgram,
                                                                     &sRiggedProgram, &sRiggedBindlessProgram };
        return *programs[which & 3];
    }
}

//-----------------------------------------------------------------------------
// Bindless texture handles

namespace
{
#if FS_GDD_BINDLESS_API
    PFNGLGETTEXTURESAMPLERHANDLEARBPROC sGetTextureSamplerHandle = nullptr;
    PFNGLMAKETEXTUREHANDLERESIDENTARBPROC sMakeResident = nullptr;
    PFNGLMAKETEXTUREHANDLENONRESIDENTARBPROC sMakeNonResident = nullptr;

    // The bindless entry points are loaded here, not through llglheaders.h, so FSRenderThread's
    // GL command stream doesn't proxy them: run them where the context is, in stream order
    // (runSync and recordCall call them directly when the stream isn't recording). Residency
    // changes return nothing, so they are recorded rather than waited for.
    GLuint64 fsGetHandle(GLuint texture, GLuint sampler)
    {
        GLuint64 handle = 0;
        FSGLStream::runSync([&]() { handle = sGetTextureSamplerHandle(texture, sampler); });
        return handle;
    }

    void fsMakeResidentNow(U64 handle)
    {
        sMakeResident((GLuint64)handle);
    }

    void fsMakeNonResidentNow(U64 handle)
    {
        sMakeNonResident((GLuint64)handle);
    }

    void fsMakeResident(GLuint64 handle)
    {
        FSGLStream::recordCall(&fsMakeResidentNow, (U64)handle);
    }

    void fsMakeNonResident(GLuint64 handle)
    {
        FSGLStream::recordCall(&fsMakeNonResidentNow, (U64)handle);
    }
#endif

    struct SamplerState
    {
        GLint mMag = GL_LINEAR;
        GLint mMin = GL_LINEAR;
        GLint mWrap = GL_REPEAT;
        F32 mAniso = 0.f;               // 0: not set (no anisotropic filtering support)

        bool operator==(const SamplerState& rhs) const
        {
            return mMag == rhs.mMag && mMin == rhs.mMin && mWrap == rhs.mWrap && mAniso == rhs.mAniso;
        }
    };

    struct HandleSlot
    {
        SamplerState mState;
        GLuint mSampler = 0;
        GLuint64 mHandle = 0;           // 0: the driver gave none at mLastFrame (no sampler then), tried again later
        bool mResident = false;
        U32 mLastFrame = 0;
        const LLImageGL* mOwner = nullptr;  // the image the handle was made for (compared, never used)
    };

    // A handle wanted while the render thread records: made with the others at the next frame's first lookup
    struct HandleRequest
    {
        LLPointer<LLViewerTexture> mTex;
        U32 mName;
        SamplerState mState;
        GLuint mSampler;                // while being made
    };

    std::mutex sHandleMutex;                                    // the hook may come from the texture thread
    std::unordered_map<U32, std::vector<HandleSlot>> sHandles;  // by texture GL name
    std::atomic<U32> sHandleTextures{ 0 };                      // sHandles.size() + sHandleQueue.size(), for the hook's fast path
    std::unordered_map<U32, std::vector<HandleRequest>> sHandleQueue;   // by texture GL name; changed on the main thread only
    U32 sHandleQueueFrame = 0;                                  // the frame the queue was last worked off

    // Handles being made on the render thread (FSGLStream::recordCall, in stream order, no wait), taken in at
    // a later frame's first lookup. The main thread owns a batch; the render thread only fills mHandles and
    // then sets mDone. A request unlocked meanwhile is cancelled and its sampler deleted right away, which in
    // stream order comes after its handle was made, so the texture is mutable again before it is changed.
    struct HandleBatch
    {
        std::vector<HandleRequest> mRequests;
        std::vector<GLuint64> mHandles;
        std::vector<U8> mCancelled;                             // main thread
        std::atomic<bool> mDone{ false };
    };
    std::vector<HandleBatch*> sHandleBatches;                   // oldest first; sHandleMutex held
    std::unordered_map<U32, std::vector<SamplerState>> sHandlesInFlight;    // by texture GL name; sHandleMutex held
    U32 sResident = 0;
    U32 sStatHandlesMade = 0;
    U32 sStatHandleFailures = 0;
    U32 sStatUnlocks = 0;
    // Why texture_handle gave no handle (per lookup: the GDD passes look a texture up once per pass), and how
    // often the handle of the image's previous texture stood in while its new one was being made
    enum ENoHandle : U32
    {
        NH_NOT_FETCHED = 0,     // not a fetched (or LOD) texture
        NH_NOT_LOADED,          // no GL texture yet
        NH_NOT_2D,
        NH_OPTIONS,             // its filtering and wrapping not applied yet (at its next stock bind)
        NH_MOVED,               // moved off a locked texture by a partial update a short while ago
        NH_COMPRESSED,
        NH_FAILED,              // the driver gave none; tried again later
        NH_PENDING,             // being made (asked for this frame or the last)
        NH_COUNT
    };
    U32 sStatNoHandle[NH_COUNT] = {};
    U32 sStatPreviousHandle = 0;
    S32 sBindlessTested = 0;                                    // 0 not yet, 1 works, -1 doesn't
    U32 sLastHandleSweep = 0;
    // Moves on whenever a handle stops being usable (dropped, or made non resident), which invalidates the
    // handles textures remember for the frame (LLViewerTexture::mFSGDDCacheHandle). Main thread only.
    U32 sHandleEpoch = 1;
    static_assert(sizeof(SamplerState) == sizeof(LLViewerTexture::mFSGDDCacheState), "cache layout");

    // As LLTexUnit::setTextureAddressModeFast and setTextureFilteringOptionFast would set the texture up.
    SamplerState sampler_state(const LLImageGL* gl)
    {
        SamplerState state;
        const LLTexUnit::eTextureFilterOptions option = gl->getFilteringOption();
        const bool mips = gl->getFSHasMipMaps();
        state.mMag = option == LLTexUnit::TFO_POINT ? GL_NEAREST : GL_LINEAR;
        if (option >= LLTexUnit::TFO_TRILINEAR && mips)
        {
            state.mMin = GL_LINEAR_MIPMAP_LINEAR;
        }
        else if (option >= LLTexUnit::TFO_BILINEAR)
        {
            state.mMin = mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
        }
        else
        {
            state.mMin = mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        }
        static const GLint wrap[] = { GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE };
        const U32 mode = (U32)gl->getAddressMode();
        state.mWrap = mode < 3 ? wrap[mode] : GL_REPEAT;
        if (gGLManager.mHasAnisotropic)
        {
            state.mAniso = (option == LLTexUnit::TFO_ANISOTROPIC && LLRender::sAnisotropicFilteringLevel > 1.f)
                               ? llclamp(LLRender::sAnisotropicFilteringLevel, 1.f, gGLManager.mMaxAnisotropy)
                               : 1.f;
        }
        return state;
    }

    // Drops every handle of a texture: no lookup finds them from now on, and they are retired (FSGLRetire):
    // made non resident and their samplers deleted (which deletes the handles) once the GPU has finished the
    // frames already recorded, which may sample them. Releasing them right away made the GPU read through a
    // handle that was gone. sHandleMutex held, main thread.
    void drop_slots(std::vector<HandleSlot>& slots)
    {
        ++sHandleEpoch;
#if FS_GDD_BINDLESS_API
        for (HandleSlot& slot : slots)
        {
            if (slot.mResident)
            {
                --sResident;
            }
            FSGLRetire::retireHandle(slot.mHandle, slot.mResident, slot.mSampler);
        }
#endif
        slots.clear();
    }

    // A texture locked for a handle it didn't get (the driver gave none, or the request was dropped) and that has
    // no other handle: unlocked, so its next change goes ahead in place instead of moving it to a copy (and,
    // for a partial update, keeping it without a handle for a while). sHandleMutex held, main thread.
    void release_unused_lock(U32 name)
    {
        if (sHandlesInFlight.count(name))
        {
            return;
        }
        auto it = sHandles.find(name);
        if (it != sHandles.end())
        {
            for (const HandleSlot& slot : it->second)
            {
                if (slot.mHandle)
                {
                    return;
                }
            }
        }
        LLImageGL::fsReleaseLock(name);
    }

    // sHandleMutex held
    void update_handle_count()
    {
        sHandleTextures.store((U32)(sHandles.size() + sHandleQueue.size() + sHandlesInFlight.size()), std::memory_order_relaxed);
    }

    // sHandleMutex held
    bool handle_in_flight(U32 name, const SamplerState& state)
    {
        auto it = sHandlesInFlight.find(name);
        if (it == sHandlesInFlight.end())
        {
            return false;
        }
        for (const SamplerState& in_flight : it->second)
        {
            if (in_flight == state)
            {
                return true;
            }
        }
        return false;
    }

    // sHandleMutex held
    void forget_in_flight(U32 name, const SamplerState& state)
    {
        auto it = sHandlesInFlight.find(name);
        if (it == sHandlesInFlight.end())
        {
            return;
        }
        std::vector<SamplerState>& states = it->second;
        for (size_t i = 0; i < states.size(); ++i)
        {
            if (states[i] == state)
            {
                states.erase(states.begin() + i);
                break;
            }
        }
        if (states.empty())
        {
            sHandlesInFlight.erase(it);
        }
    }

    // Cancels the requests of tex_name (of every texture with 0) still being made: their samplers are
    // deleted, which deletes the handles once made. sHandleMutex held, main thread.
    void cancel_in_flight(U32 tex_name)
    {
#if FS_GDD_BINDLESS_API
        if (sHandlesInFlight.empty())
        {
            return;
        }
        for (HandleBatch* batch : sHandleBatches)
        {
            for (size_t i = 0; i < batch->mRequests.size(); ++i)
            {
                HandleRequest& request = batch->mRequests[i];
                if (batch->mCancelled[i] || (tex_name && request.mName != tex_name))
                {
                    continue;
                }
                batch->mCancelled[i] = 1;
                glDeleteSamplers(1, &request.mSampler);
                forget_in_flight(request.mName, request.mState);
            }
        }
#endif
    }

    // LLImageGL::sFSTextureUnlock: tex_name is about to be respecified or get new parameters.
    void unlock_texture(U32 tex_name)
    {
        if (!sHandleTextures.load(std::memory_order_relaxed))
        {
            return;
        }
        // Released after the lock: dropping the last reference to a texture deletes it, which comes back here
        std::vector<HandleRequest> dropped;
        std::lock_guard<std::mutex> lock(sHandleMutex);
        auto it = sHandles.find(tex_name);
        if (!on_main_thread())
        {   // handles are only made for textures the main thread owns; the texture thread works on new names
            // (and a queued request, which holds a texture reference, is checked again before its handle is made)
            if (it != sHandles.end())
            {
                LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenPBRBindless: texture " << tex_name
                                           << " with bindless handles changed off the main thread" << LL_ENDL;
            }
            return;
        }
        auto queued = sHandleQueue.find(tex_name);
        if (queued != sHandleQueue.end())
        {   // respecified or deleted before its handle was made
            dropped.swap(queued->second);
            sHandleQueue.erase(queued);
            update_handle_count();
        }
        if (sHandlesInFlight.count(tex_name))
        {   // respecified or deleted while its handle is being made
            cancel_in_flight(tex_name);
            update_handle_count();
        }
        if (it == sHandles.end())
        {
            return;
        }
        drop_slots(it->second);
        sHandles.erase(it);
        update_handle_count();
        ++sStatUnlocks;
    }

    void release_handles()
    {
        std::unordered_map<U32, std::vector<HandleRequest>> queue;  // released after the lock (unlock_texture)
        std::vector<std::unique_ptr<HandleBatch>> finished;         // as queue
        std::lock_guard<std::mutex> lock(sHandleMutex);
        queue.swap(sHandleQueue);
        cancel_in_flight(0);
        for (auto it = sHandleBatches.begin(); it != sHandleBatches.end(); )
        {   // a batch the render thread hasn't got to yet stays until it has
            if ((*it)->mDone.load(std::memory_order_acquire))
            {
                finished.emplace_back(*it);
                it = sHandleBatches.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto& texture : sHandles)
        {
            drop_slots(texture.second);
        }
        sHandles.clear();
        sHandleTextures.store(0, std::memory_order_relaxed);
        sResident = 0;
        // Rare (shader reloads, the GL going away): wait for the GPU in stream order and release them all now
        FSGLRetire::finish();
    }

    // Once per GL context: does deleting the sampler of a texture/sampler handle make the texture mutable
    // again, as GL_ARB_bindless_texture says? Without that, the textures LLImageGL updates would break.
    bool bindless_self_test()
    {
#if FS_GDD_BINDLESS_API
        sGetTextureSamplerHandle = (PFNGLGETTEXTURESAMPLERHANDLEARBPROC)gGLManager.mFSBindlessProcs[0];
        sMakeResident = (PFNGLMAKETEXTUREHANDLERESIDENTARBPROC)gGLManager.mFSBindlessProcs[1];
        sMakeNonResident = (PFNGLMAKETEXTUREHANDLENONRESIDENTARBPROC)gGLManager.mFSBindlessProcs[2];
        if (!sGetTextureSamplerHandle || !sMakeResident || !sMakeNonResident)
        {
            return false;
        }
        for (U32 i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        {
        }

        GLint previous = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        const GLuint64 handle = fsGetHandle(tex, sampler);
        // <FS:Trish> Which step failed, with its GL error, logged below where it isn't filtered out.
        // Textures stay immutable once they had a handle (LLImageGL moves to a copy before changing one),
        // so only making, using and releasing handles is tested.
        const char* failed_step = nullptr;
        GLenum failed_error = glGetError();
        bool ok = handle != 0 && failed_error == GL_NO_ERROR;
        if (!ok)
        {
            failed_step = "making the handle";
        }
        else
        {
            fsMakeResident(handle);
            fsMakeNonResident(handle);
            glDeleteSamplers(1, &sampler);
            sampler = 0;
            failed_error = glGetError();
            if (failed_error != GL_NO_ERROR)
            {
                failed_step = "residency or deleting the sampler";
            }
            ok = !failed_step;
        }
        if (failed_step)
        {
            LL_WARNS("Rendering") << "Bindless texture self test failed at " << failed_step << " (GL error 0x"
                                  << std::hex << failed_error << std::dec << ", handle " << handle
                                  << "); GPU-driven passes bind textures instead. Driver: " << gGLManager.mDriverVersionVendorString
                                  << LL_ENDL;
        }
        else
        {
            LL_INFOS("Rendering") << "Bindless texture self test passed" << LL_ENDL;
            FSGLRetire::setNonResidentFunc(&fsMakeNonResidentNow); // retired handles go non resident through it
        }
        // </FS:Trish>
        if (sampler)
        {
            glDeleteSamplers(1, &sampler);
        }
        glBindTexture(GL_TEXTURE_2D, previous);
        glDeleteTextures(1, &tex);
        for (U32 i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i)
        {
        }
        gGL.invalidateFastBinds();
        return ok;
#else
        return false;
#endif
    }

#if FS_GDD_BINDLESS_API
    // A sampler set up as state; recorded, no wait
    GLuint make_sampler(const SamplerState& state)
    {
        GLuint sampler = 0;
        glGenSamplers(1, &sampler);
        glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, state.mMag);
        glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, state.mMin);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, state.mWrap);
        glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, state.mWrap);
        if (state.mAniso > 0.f)
        {
            glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, state.mAniso);
        }
        return sampler;
    }

    // Files a handle just made under name, or (handle 0) that the driver gave none, so the pair isn't asked
    // for again every frame. sHandleMutex held, main thread.
    void add_slot(U32 name, const SamplerState& state, GLuint sampler, GLuint64 handle, U32 frame, const LLImageGL* owner)
    {
        HandleSlot slot;
        slot.mState = state;
        slot.mLastFrame = frame;
        slot.mOwner = owner;
        if (!handle)
        {
            glDeleteSamplers(1, &sampler);
            ++sStatHandleFailures;
            sHandles[name].push_back(slot);
            release_unused_lock(name);
            return;
        }
        else
        {
            slot.mSampler = sampler;
            slot.mHandle = handle;
            fsMakeResident(handle);
            slot.mResident = true;
            ++sResident;
            ++sStatHandlesMade;
        }
        sHandles[name].push_back(slot);
    }

    // Render thread (directly when not recording), in stream order: makes the handles of a batch.
    void make_handles_gl(U64 ptr)
    {
        HandleBatch* batch = reinterpret_cast<HandleBatch*>((uintptr_t)ptr);
        for (size_t i = 0; i < batch->mRequests.size(); ++i)
        {
            // 0 (and GL_INVALID_OPERATION) for a texture that isn't complete with this sampler
            batch->mHandles[i] = sGetTextureSamplerHandle(batch->mRequests[i].mName, batch->mRequests[i].mSampler);
            if (!batch->mHandles[i])
            {
                for (U32 j = 0; j < 4 && glGetError() != GL_NO_ERROR; ++j)
                {
                }
            }
        }
        batch->mDone.store(true, std::memory_order_release);
    }

    // Files the handles of the batches the render thread has made, oldest first, of the textures still as
    // they were asked for. The batches go to finished, to be deleted after the lock: dropping the last
    // reference to a texture deletes it, which calls unlock_texture. sHandleMutex held, main thread.
    void take_made_handles(U32 frame, std::vector<std::unique_ptr<HandleBatch>>& finished)
    {
        while (!sHandleBatches.empty() && sHandleBatches.front()->mDone.load(std::memory_order_acquire))
        {
            HandleBatch* batch = sHandleBatches.front();
            sHandleBatches.erase(sHandleBatches.begin());
            for (size_t i = 0; i < batch->mRequests.size(); ++i)
            {
                if (batch->mCancelled[i])
                {   // its sampler is already deleted
                    continue;
                }
                HandleRequest& request = batch->mRequests[i];
                forget_in_flight(request.mName, request.mState);
                LLImageGL* gl = request.mTex->getGLTexture();
                bool keep = gl && gl->getTexName() == request.mName && !gl->getFSTexOptionsDirty() &&
                            sampler_state(gl) == request.mState;
                auto it = sHandles.find(request.mName);
                if (keep && it != sHandles.end())
                {
                    for (const HandleSlot& slot : it->second)
                    {
                        keep = keep && !(slot.mState == request.mState);
                    }
                }
                if (keep)
                {
                    add_slot(request.mName, request.mState, request.mSampler, batch->mHandles[i], frame, gl);
                }
                else
                {   // changed since or made twice: deleting the sampler deletes the handle
                    glDeleteSamplers(1, &request.mSampler);
                    release_unused_lock(request.mName);
                }
            }
            finished.emplace_back(batch);
        }
    }

    // The handles asked for while the render thread recorded, of the textures still as they were then, all
    // made together on the render thread without waiting for it; they are filed at a later frame's first
    // lookup (the batch takes bound textures meanwhile). Once per frame, at its first lookup or sweep. Main
    // thread.
    void make_queued_handles(U32 frame)
    {
        if (frame == sHandleQueueFrame)
        {
            return;
        }
        sHandleQueueFrame = frame;
        // released after the lock: dropping the last reference to a texture deletes it, which calls unlock_texture
        std::unordered_map<U32, std::vector<HandleRequest>> queue;
        std::vector<std::unique_ptr<HandleBatch>> finished;
        std::lock_guard<std::mutex> lock(sHandleMutex);
        take_made_handles(frame, finished);
        if (sHandleQueue.empty())
        {
            update_handle_count();
            return;
        }
        queue.swap(sHandleQueue);
        HandleBatch* batch = new HandleBatch;
        for (auto& texture : queue)
        {
            for (HandleRequest& request : texture.second)
            {
                LLImageGL* gl = request.mTex->getGLTexture();
                // one and two channel textures too: their swizzle is texture state, which the handle keeps
                if (!gl || gl->getTexName() != request.mName || gl->getFSTexOptionsDirty() ||
                    gl->getTarget() != LLTexUnit::TT_TEXTURE || !(sampler_state(gl) == request.mState))
                {   // changed since: asked for again if still wanted
                    continue;
                }
                bool made = handle_in_flight(request.mName, request.mState);
                auto it = sHandles.find(request.mName);
                if (it != sHandles.end())
                {
                    for (const HandleSlot& slot : it->second)
                    {
                        made = made || slot.mState == request.mState;
                    }
                }
                if (!made)
                {
                    LLImageGL::fsLockTexture(request.mName, gl); // <FS:Trish/> immutable once the render thread makes the handle
                    request.mSampler = make_sampler(request.mState);
                    sHandlesInFlight[request.mName].push_back(request.mState);
                    batch->mRequests.push_back(request);
                }
            }
        }

        if (batch->mRequests.empty())
        {
            delete batch;
        }
        else
        {
            batch->mHandles.assign(batch->mRequests.size(), 0);
            batch->mCancelled.assign(batch->mRequests.size(), 0);
            sHandleBatches.push_back(batch);
            FSGLStream::recordCall(&make_handles_gl, (U64)(uintptr_t)batch);
            // made right away when the render thread isn't recording
            take_made_handles(frame, finished);
        }
        update_handle_count();
    }
#endif

    // While the image's new texture has no handle yet (asked for, made over the next frame or two), the handle
    // of its previous texture, if that one is still there (in LLImageGL's delete queue until the start of the
    // next frame, which then drops its handles; they and the texture are released once the GPU has finished
    // the frames that used them, FSGLRetire) and was sampled the same way: the batch keeps its multi-draw
    // instead of falling back to the stock path, drawing the previous levels (the same image, at the old
    // resolution) for those frames. sHandleMutex held, main thread.
    GLuint64 previous_handle(LLViewerTexture* tex, const LLImageGL* gl, U32 name, const SamplerState& state, U32 frame)
    {
        const U32 previous = tex->mFSGDDCacheName;
        if (!previous || previous == name || memcmp(tex->mFSGDDCacheState, &state, sizeof(state)))
        {
            return 0;
        }
        auto it = sHandles.find(previous);
        if (it == sHandles.end())
        {
            return 0;
        }
        for (HandleSlot& slot : it->second)
        {
            if (slot.mHandle && slot.mOwner == gl && slot.mState == state)
            {
#if FS_GDD_BINDLESS_API
                if (!slot.mResident)
                {
                    fsMakeResident(slot.mHandle);
                    slot.mResident = true;
                    ++sResident;
                }
#endif
                slot.mLastFrame = frame;
                ++sStatPreviousHandle;
                return slot.mHandle;
            }
        }
        return 0;
    }

    // The resident handle of tex as the stock path would sample it, or 0 if it can't have one. While the
    // render thread records, a missing handle is queued (0 this time) and made at the next frame's first
    // lookup together with the others: one wait for the render thread per frame, not one per texture.
    GLuint64 texture_handle(LLViewerTexture* tex, U32 frame)
    {
#if FS_GDD_BINDLESS_API
        if (!tex)
        {
            return 0;
        }
        const S8 type = tex->getType();
        LLImageGL* gl = tex->getGLTexture();
        if (type != LLViewerTexture::FETCHED_TEXTURE && type != LLViewerTexture::LOD_TEXTURE)
        {
            ++sStatNoHandle[NH_NOT_FETCHED];
            return 0;
        }
        const U32 name = gl ? gl->getTexName() : 0;
        // <FS:Trish> nor for a texture that moved off a locked one (it keeps changing), nor a compressed one; one
        // and two channel textures do get handles (their swizzle is texture state, which the handle keeps)
        //if (!name || gl->getTarget() != LLTexUnit::TT_TEXTURE || gl->getComponents() < 3 || gl->getFSTexOptionsDirty())
        if (!name || gl->getTarget() != LLTexUnit::TT_TEXTURE || gl->getFSTexOptionsDirty() ||
            gl->getFSNoBindless() || gl->fsIsCompressed())
        // </FS:Trish>
        {   // a texture with pending options gets them at its next bind, which would drop its handle mid pass
            const ENoHandle reason = !name ? NH_NOT_LOADED
                                   : gl->getTarget() != LLTexUnit::TT_TEXTURE ? NH_NOT_2D
                                   : gl->getFSTexOptionsDirty() ? NH_OPTIONS
                                   : gl->getFSNoBindless() ? NH_MOVED
                                   : NH_COMPRESSED;
            ++sStatNoHandle[reason];
            return 0;
        }
        const SamplerState state = sampler_state(gl);
        // Every pass of the frame asks again, for every material: after the first, no lock and no table search
        if (tex->mFSGDDCacheFrame == frame && tex->mFSGDDCacheEpoch == sHandleEpoch && tex->mFSGDDCacheName == name &&
            !memcmp(tex->mFSGDDCacheState, &state, sizeof(state)))
        {
            return tex->mFSGDDCacheHandle;
        }
        auto remember = [tex, frame, name, &state](GLuint64 handle)
        {
            tex->mFSGDDCacheFrame = frame;
            tex->mFSGDDCacheEpoch = sHandleEpoch;
            tex->mFSGDDCacheName = name;
            memcpy(tex->mFSGDDCacheState, &state, sizeof(state));
            tex->mFSGDDCacheHandle = handle;
            return handle;
        };
        make_queued_handles(frame);

        std::lock_guard<std::mutex> lock(sHandleMutex);
        auto it = sHandles.find(name);
        if (it != sHandles.end())
        {
            std::vector<HandleSlot>& slots = it->second;
            for (size_t i = 0; i < slots.size(); ++i)
            {
                HandleSlot& slot = slots[i];
                if (!(slot.mState == state))
                {
                    continue;
                }
                if (!slot.mHandle)
                {   // the driver gave none
                    if (frame - slot.mLastFrame < FS_GDD_HANDLE_RETRY_FRAMES)
                    {
                        ++sStatNoHandle[NH_FAILED];
                        return 0;
                    }
                    slots.erase(slots.begin() + i);
                    if (slots.empty())
                    {
                        sHandles.erase(it);
                        update_handle_count();
                    }
                    break;
                }
                if (!slot.mResident)
                {
                    fsMakeResident(slot.mHandle);
                    slot.mResident = true;
                    ++sResident;
                }
                slot.mLastFrame = frame;
                return remember(slot.mHandle);
            }
        }

        if (!FSGLStream::isRecording())
        {   // GL is on this thread: made right away, nothing to wait for
            LLImageGL::fsLockTexture(name, gl); // <FS:Trish/> immutable from the handle on
            const GLuint sampler = make_sampler(state);
            // 0 (and GL_INVALID_OPERATION) for a texture that isn't complete with this sampler
            const GLuint64 handle = sGetTextureSamplerHandle(name, sampler);
            if (!handle)
            {
                for (U32 i = 0; i < 4 && glGetError() != GL_NO_ERROR; ++i)
                {
                }
            }
            add_slot(name, state, sampler, handle, frame, gl);
            update_handle_count();
            if (!handle)
            {
                ++sStatNoHandle[NH_FAILED];
            }
            return handle;
        }

        bool asked = handle_in_flight(name, state);
        auto queued = sHandleQueue.find(name);
        if (!asked && queued != sHandleQueue.end())
        {
            for (const HandleRequest& request : queued->second)
            {
                asked = asked || request.mState == state;
            }
        }
        if (!asked)
        {
            sHandleQueue[name].push_back({ tex, name, state, 0 });
            update_handle_count();
        }
        const GLuint64 previous = previous_handle(tex, gl, name, state, frame);
        if (!previous)
        {
            ++sStatNoHandle[NH_PENDING];
        }
        return previous;
#else
        return 0;
#endif
    }

    // Every so often: handles not used for a while go non resident, and later away.
    void sweep_handles(U32 frame)
    {
#if FS_GDD_BINDLESS_API
        make_queued_handles(frame);
#endif
        if (frame - sLastHandleSweep < 120)
        {
            return;
        }
        sLastHandleSweep = frame;
#if FS_GDD_BINDLESS_API
        std::lock_guard<std::mutex> lock(sHandleMutex);
        for (auto it = sHandles.begin(); it != sHandles.end(); )
        {
            std::vector<HandleSlot>& slots = it->second;
            for (size_t i = 0; i < slots.size(); )
            {
                HandleSlot& slot = slots[i];
                const U32 idle = frame - slot.mLastFrame;
                if (idle >= FS_GDD_HANDLE_DROP_FRAMES)
                {
                    std::vector<HandleSlot> one(1, slot);
                    drop_slots(one);
                    slots.erase(slots.begin() + i);
                    continue;
                }
                if (idle >= FS_GDD_HANDLE_IDLE_FRAMES && slot.mResident)
                {
                    ++sHandleEpoch;
                    fsMakeNonResident(slot.mHandle);
                    slot.mResident = false;
                    --sResident;
                }
                ++i;
            }
            it = slots.empty() ? sHandles.erase(it) : std::next(it);
        }
        update_handle_count();
#endif
    }
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    // stock: gDeferredPBROpaqueProgram, or (rigged) its skinned variant
    bool create_program(LLGLSLShader& shader, const LLGLSLShader& stock, const char* name, bool bindless, bool rigged)
    {
        shader.mName = name;
        shader.mFeatures = stock.mFeatures;
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_EMISSIVE, FS_SPECULAR_AA, ...), then ours
        shader.addPermutation("FS_GDD", "1");
        if (bindless)
        {
            shader.addPermutation("FS_GDD_BINDLESS", "1");
        }
        // rigged (HAS_SKIN among the stock defines): the skinning object that reads the palettes from the storage buffer
        shader.mFeatures.mFSGDDSkinning = rigged;
        return shader.createShader();
    }

    // the storage block bindings, and where the driver put the per command record
    bool bind_inputs(LLGLSLShader& shader, bool rigged, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDPBRMatrices");
        const GLuint materials = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDPBRMaterials");
        const GLuint palettes = rigged ? glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDSkinPalettes")
                                       : GL_INVALID_INDEX;
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || materials == GL_INVALID_INDEX || (rigged && palettes == GL_INVALID_INDEX) ||
            record_attrib < 0 || (rigged && !(shader.mAttributeMask & LLVertexBuffer::MAP_WEIGHT4)))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_PBR_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, materials, FS_GDD_PBR_MATERIAL_BINDING);
        if (rigged)
        {
            glShaderStorageBlockBinding(shader.mProgramObject, palettes, FS_GDD_PBR_SKIN_BINDING);
        }
        return true;
    }

    // Binds a GPU-driven program; the first time, also finds its inputs. False (for good) when they aren't there.
    bool bind_program(U32 which)
    {
        LLGLSLShader& shader = program(which);
        shader.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &shader)
        {
            return false;
        }
        if (!sInputsReady[which])
        {
            sInputsReady[which] = bind_inputs(shader, (which & FS_GDD_PBR_RIGGED) != 0, sRecordAttrib[which]) ? 1 : -1;
            if (sInputsReady[which] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenPBR: " << shader.mName << " lacks its storage blocks or draw "
                                      << "record attribute, PBR batches are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[which] > 0;
    }

    bool program_usable(U32 which)
    {
        return program(which).isComplete() && sInputsReady[which] >= 0;
    }
}

void fsGDDPBRUnloadShaders()
{
    for (U32 which = 0; which < FS_GDD_PBR_PROGRAMS; ++which)
    {
        program(which).unload();
        sInputsReady[which] = 0;
        sRecordAttrib[which] = -1;
    }
    fsGDDPBRReleaseGL();
}

void fsGDDPBRLoadShaders()
{
    fsGDDPBRUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenPBR", false);
    static LLCachedControl<bool> bindless(gSavedSettings, "FSPerfGPUDrivenPBRBindless", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenPBR: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; PBR batches are drawn as before" << LL_ENDL;
        return;
    }
    // mFSHasGPUDrivenDraws asks for one vertex shader storage block (the shadow pass); this program uses two
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenPBR: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; PBR batches are drawn as before" << LL_ENDL;
        return;
    }
    if (gDeferredPBROpaqueProgram.mShaderFiles.empty())
    {
        return;
    }

    if (!create_program(gFSGDDPBRProgram, gDeferredPBROpaqueProgram, "FS GPU-Driven PBR Opaque Shader", false, false))
    {
        LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenPBR: the multi-draw PBR program failed to build, "
                                  << "PBR batches are drawn as before" << LL_ENDL;
        fsGDDPBRUnloadShaders();
        return;
    }

    if (bindless && !gGLManager.mIsNVIDIA)
    {   // only tested on NVIDIA; elsewhere drivers may want handles uniform within a multi-draw wave,
        // which the commands of a batch don't guarantee
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenPBRBindless: only used on NVIDIA GPUs, "
                                       << "PBR textures are bound as before" << LL_ENDL;
    }
    else if (bindless)
    {
        if (!sBindlessTested)
        {
            sBindlessTested = bindless_self_test() ? 1 : -1;
            LL_INFOS("ShaderLoading") << "FSPerfGPUDrivenPBRBindless: bindless textures "
                                      << (sBindlessTested > 0 ? "usable" : "not usable (missing, or the driver keeps textures "
                                                                           "immutable after their handles are gone)")
                                      << LL_ENDL;
        }
        if (sBindlessTested > 0)
        {
            if (create_program(gFSGDDPBRBindlessProgram, gDeferredPBROpaqueProgram, "FS GPU-Driven PBR Opaque Bindless Shader",
                               true, false))
            {
                LLImageGL::sFSTextureUnlock = &unlock_texture;  // stays set; does nothing while there are no handles
            }
            else
            {
                LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenPBRBindless: the bindless program failed to build, "
                                          << "PBR textures are bound as before" << LL_ENDL;
                gFSGDDPBRBindlessProgram.unload();
            }
        }
    }

    // FSPerfGPUDrivenRigged: the skinned programs, from the stock skinned one; the bindless one only next to the
    // static bindless one. Optional: without them the rigged passes stay stock.
    const LLGLSLShader* stock_rigged = gDeferredPBROpaqueProgram.mRiggedVariant;
    if (stock_rigged && stock_rigged != &gDeferredPBROpaqueProgram && !stock_rigged->mShaderFiles.empty() &&
        fsGDDRiggedLoadable(vs_blocks))
    {
        if (!create_program(sRiggedProgram, *stock_rigged, "FS GPU-Driven Skinned PBR Opaque Shader", false, true))
        {
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenRigged: the skinned multi-draw PBR program failed to build, "
                                      << "rigged PBR batches are drawn as before" << LL_ENDL;
            sRiggedProgram.unload();
        }
        else if (gFSGDDPBRBindlessProgram.isComplete() &&
                 !create_program(sRiggedBindlessProgram, *stock_rigged, "FS GPU-Driven Skinned PBR Opaque Bindless Shader",
                                 true, true))
        {
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenRigged: the skinned bindless PBR program failed to build, "
                                      << "rigged PBR textures are bound as before" << LL_ENDL;
            sRiggedBindlessProgram.unload();
        }
    }
}

void fsGDDPBRReleaseGL()
{
    sArena.releaseGL();
    sRiggedArena.releaseGL();
    sIndirectBuffer.release();
    sRecordBuffer.release();
    sMaterialBuffer.release();
    sMatrices.release();
    fsGDDDeleteQuery(sQuery);
    release_handles();
    sBindlessTested = 0;    // per GL context: tested again at the next load
}

//-----------------------------------------------------------------------------
// Bindless handles for FSPerfGPUDrivenLegacy (fsgpudrivenlegacy.cpp), shared with the PBR pass

bool fsGDDBindlessSetup()
{
    if (!gGLManager.mIsNVIDIA)
    {   // as FSPerfGPUDrivenPBRBindless: only tested on NVIDIA
        return false;
    }
    if (!sBindlessTested)
    {
        sBindlessTested = bindless_self_test() ? 1 : -1;
        LL_INFOS("ShaderLoading") << "GPU-driven draws: bindless textures "
                                  << (sBindlessTested > 0 ? "usable" : "not usable (missing, or the driver keeps textures "
                                                                       "immutable after their handles are gone)")
                                  << LL_ENDL;
    }
    if (sBindlessTested > 0)
    {
        LLImageGL::sFSTextureUnlock = &unlock_texture;  // stays set; does nothing while there are no handles
    }
    return sBindlessTested > 0;
}

bool fsGDDBindlessReady()
{
    return sBindlessTested > 0;
}

U64 fsGDDTextureHandle(LLViewerTexture* tex, U32 frame)
{
    return (U64)texture_handle(tex, frame);
}

void fsGDDSweepHandles(U32 frame)
{
    sweep_handles(frame);
}

//-----------------------------------------------------------------------------
// The pass

namespace
{
    // The material's record: what LLFetchedGLTFMaterial::bind(nullptr) sets in the deferred pass, and, when
    // bindless is wanted and every texture can have a handle, the handles of what it would bind.
    MaterialInfo material_index(LLFetchedGLTFMaterial* mat, bool want_bindless, U32 frame)
    {
        if (mat->mFSGDDPass == sMaterialPass)
        {
            return { mat->mFSGDDIndex, mat->mFSGDDBindless };
        }
        Material record = {};
        static const LLGLTFMaterial::TextureInfo infos[4] = { LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR,
                                                              LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL,
                                                              LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS,
                                                              LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE };
        for (U32 i = 0; i < 4; ++i)
        {
            F32 packed[8];
            mat->mTextureTransform[infos[i]].getPacked(packed);
            memcpy(record.mTransform[i * 2], packed, sizeof(packed));
        }
        record.mFactors[0] = mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? mat->mAlphaCutoff : -1.f;
        record.mFactors[1] = mat->mRoughnessFactor;
        record.mFactors[2] = mat->mMetallicFactor;
        record.mEmissive[0] = mat->mEmissiveColor.mV[0];
        record.mEmissive[1] = mat->mEmissiveColor.mV[1];
        record.mEmissive[2] = mat->mEmissiveColor.mV[2];

        bool bindless = want_bindless;
        if (bindless)
        {   // the textures bind() picks without a media override
            LLViewerTexture* textures[4] =
            {
                mat->mBaseColorTexture.notNull() ? (LLViewerTexture*)mat->mBaseColorTexture.get() : LLViewerFetchedTexture::sWhiteImagep.get(),
                (mat->mNormalTexture.notNull() && mat->mNormalTexture->getDiscardLevel() <= 4)
                    ? (LLViewerTexture*)mat->mNormalTexture.get() : LLViewerFetchedTexture::sFlatNormalImagep.get(),
                mat->mMetallicRoughnessTexture.notNull() ? (LLViewerTexture*)mat->mMetallicRoughnessTexture.get() : LLViewerFetchedTexture::sWhiteImagep.get(),
                mat->mEmissiveTexture.notNull() ? (LLViewerTexture*)mat->mEmissiveTexture.get() : LLViewerFetchedTexture::sWhiteImagep.get(),
            };
            for (U32 i = 0; i < 4 && bindless; ++i)
            {
                const GLuint64 handle = texture_handle(textures[i], frame);
                bindless = handle != 0;
                record.mTex[i * 2] = (U32)(handle & 0xFFFFFFFFull);
                record.mTex[i * 2 + 1] = (U32)(handle >> 32);
            }
            if (bindless)
            {
                for (LLViewerTexture* tex : textures)
                {
                    tex->setActive();   // as LLTexUnit::bindFast does, so the texture stays wanted
                    // and counts as bound this frame (isJustBound, getBoundRecently), as a real bind would
                    if (LLImageGL* gl = tex->getGLTexture())
                    {
                        gl->updateBindStats();
                    }
                }
            }
        }
        const MaterialInfo info = { (U32)sMaterials.size(), bindless };
        sMaterials.push_back(record);
        mat->mFSGDDPass = sMaterialPass;
        mat->mFSGDDIndex = info.mIndex;
        mat->mFSGDDBindless = info.mBindless;
        return info;
    }

    void count(U32 reason)
    {
        ++sStatReasons[reason];
    }

    // FSPerfGPUDrivenPBRGroupRuns: the batches of the pass, collected before they become commands
    struct Pending
    {
        LLDrawInfo* mParams;
        LLFetchedGLTFMaterial* mMat;
        LLViewerTexture* mTex;      // media override
        GLuint mCount;
        GLuint mFirstIndex;
        GLint mBaseVertex;
        Record mRecord;
        EOpKind mKind;
        bool mDoubleSided;
        U32 mSeq;                   // stock order, the last sort key (std::sort without stable_sort's buffer)
    };
    std::vector<Pending> sPending;
    std::vector<LLDrawInfo*> sStockParams;

    // Grouped order: all bindless batches as one multi-draw per face culling state, then the bound ones as
    // one run per material (and media override and culling state), then the stock fallbacks. Within a group
    // batches keep their stock order (the render map's material sort).
    void emit_grouped()
    {
        auto emit_run = [](std::vector<Pending>::const_iterator first, std::vector<Pending>::const_iterator last)
        {
            const U32 start = (U32)sCommands.size();
            for (auto it = first; it != last; ++it)
            {
                const U32 command = (U32)sCommands.size();
                sCommands.push_back({ it->mCount, 1, it->mFirstIndex, it->mBaseVertex, command });
                sRecords.push_back(it->mRecord);
                sCommandParams.push_back(it->mParams);
            }
            sOps.push_back({ first->mKind, first->mDoubleSided, start, (U32)(last - first),
                             first->mKind == OP_BOUND ? first->mMat : nullptr, first->mKind == OP_BOUND ? first->mTex : nullptr,
                             nullptr });
        };

        std::sort(sPending.begin(), sPending.end(), [](const Pending& a, const Pending& b)
        {
            if (a.mKind != b.mKind)
            {
                return a.mKind < b.mKind;   // OP_BINDLESS first
            }
            if (a.mKind == OP_BOUND)
            {
                if (a.mMat != b.mMat)
                {
                    return a.mMat < b.mMat;
                }
                if (a.mTex != b.mTex)
                {
                    return a.mTex < b.mTex;
                }
            }
            if (a.mDoubleSided != b.mDoubleSided)
            {
                return a.mDoubleSided < b.mDoubleSided;
            }
            return a.mSeq < b.mSeq;
        });

        auto first = sPending.cbegin();
        for (auto it = sPending.cbegin(); it != sPending.cend(); ++it)
        {
            if (it->mKind != first->mKind || it->mDoubleSided != first->mDoubleSided ||
                (it->mKind == OP_BOUND && (it->mMat != first->mMat || it->mTex != first->mTex)))
            {
                emit_run(first, it);
                first = it;
            }
        }
        if (first != sPending.cend())
        {
            emit_run(first, sPending.cend());
        }
        for (LLDrawInfo* params : sStockParams)
        {
            sOps.push_back({ OP_STOCK, false, 0, 0, nullptr, nullptr, params });
        }
    }

    // Turns the pass's batches into ops (in the stock order, or grouped) and commands.
    void build(LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end, bool bindless, bool group_runs)
    {
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD);
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD_PBR);
        FSGDDBuildScope scope(FS_GDD_BUILDER_PBR);
        scope.addBatches((size_t)(end - begin));
        sPending.clear();
        sStockParams.clear();
        sStockRigged.clear();
        sOps.clear();
        sCommands.clear();
        sRecords.clear();
        sMaterials.clear();
        sCommandParams.clear();
        if (++sMaterialPass == 0)
        {
            sMaterialPass = 1; // 0 is what a material starts with
        }

        const U32 frame = LLFrameTimer::getFrameCount();
        FSGDDArena& arena = *sPassArena;
        arena.beginFrame(frame);
        const glm::mat4 proj = FSReverseZ::rasterProjection(gGL.getProjectionMatrix()); // as LLRender::syncMatrices gives the shaders (FSRenderReverseZ)
        const glm::mat4 view = get_current_modelview();
        sMatrices.begin(proj, view, true, true);   // texture matrix 0: identity

        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            arena.prefetchAhead(i, end - i);
            LLDrawInfo& params = **i;
            LLCullResult::increment_iterator(i, end);
            ++sStatBatches;

            // rigged: as pushRiggedGLTFBatch, which draws nothing (and binds nothing) for a batch without avatar,
            // skin or palette yet
            if (sRiggedPass && !FSSkinPaletteBuffer::drawable(params))
            {
                continue;
            }

            LLFetchedGLTFMaterial* mat = params.mGLTFMaterial.get();
            S32 reason = -1;
            const FSGDDArenaEntry* entry = nullptr;
            if (!mat)
            {
                reason = R_NO_MATERIAL;
            }
            else if (!sRiggedPass && (params.mAvatar.notNull() || params.mSkinInfo.notNull()))
            {
                reason = R_RIGGED;
            }
            else if (!params.mCount || params.mVertexBuffer.isNull())
            {
                reason = R_EMPTY;
            }
            else if (LLRenderPass::fsFadeInHidden(params) != 0.f)
            {   // <FS:Trish/> fade-in, or out (FSRenderFadeLOD): its fs_fade_hide uniform is set per batch, the stock way
                reason = R_FADING;
            }
            else
            {
                fsGDDLap(FS_GDD_STAGE_WALK);
                U32 fail = 0;
                entry = arena.find(params.mVertexBuffer.get(), frame, &fail);
                if (!entry)
                {
                    reason = R_ARENA + fail;
                }
                else if (params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
                {   // never for a valid batch; anything else is left to the stock path rather than read past the copy
                    reason = R_RANGE;
                }
                fsGDDLap(FS_GDD_STAGE_ARENA);
            }

            if (reason >= 0)
            {
                count(reason);
                if (sRiggedPass)
                {
                    sStockRigged.push_back(&params);
                }
                if (group_runs)
                {
                    sStockParams.push_back(&params);
                }
                else
                {
                    sOps.push_back({ OP_STOCK, false, 0, 0, nullptr, nullptr, &params });
                }
                continue;
            }

            // the media override replaces the base colour and emissive textures (bind()), so such a batch binds
            // its textures even when its material has handles
            LLViewerTexture* media = params.mTexture.get();
            const MaterialInfo info = material_index(mat, bindless, frame);
            EOpKind kind = OP_BOUND;
            if (info.mBindless && !media)
            {
                kind = OP_BINDLESS;
                ++sStatBindless;
            }
            else
            {
                ++sStatBound;
                if (bindless)
                {
                    count(media ? R_BOUND_MEDIA : R_BOUND_TEXTURE);
                }
            }
            const bool double_sided = mat->mDoubleSided;
            fsGDDLap(FS_GDD_STAGE_TEXTURES);
            const U32 model_matrix = sMatrices.modelIndex(params.mModelMatrix, proj, view);
            // skin SSS: the skin exclusion rides in the palette offset's top bit (assign_palettes keeps it)
            const U32 skin_exclude = sRiggedPass && LLRenderPass::fsSkinExcluded(params) ? FS_GDD_SKIN_EXCLUDE_BIT : 0;
            const Record record = { model_matrix, sMatrices.textureIndex(params.mTextureMatrix.get()), info.mIndex, skin_exclude };
            fsGDDLap(FS_GDD_STAGE_MATRICES);

            if (group_runs)
            {
                sPending.push_back({ &params, mat, media, params.mCount, entry->mIndex + params.mOffset, (GLint)entry->mVertex,
                                     record, kind, double_sided, (U32)sPending.size() });
                fsGDDLap(FS_GDD_STAGE_RECORDS);
                continue;
            }

            const U32 command = (U32)sCommands.size();
            sCommands.push_back({ params.mCount, 1, entry->mIndex + params.mOffset, (GLint)entry->mVertex, command });
            sRecords.push_back(record);
            sCommandParams.push_back(&params);

            Op* last = sOps.empty() ? nullptr : &sOps.back();
            if (last && last->mKind == kind && last->mDoubleSided == double_sided &&
                (kind == OP_BINDLESS || (last->mMat == mat && last->mTex == media)))
            {
                ++last->mCount;
            }
            else
            {
                sOps.push_back({ kind, double_sided, command, 1, mat, media, nullptr });
            }
            fsGDDLap(FS_GDD_STAGE_RECORDS);
        }

        if (group_runs)
        {
            emit_grouped();
            fsGDDLap(FS_GDD_STAGE_RECORDS);
        }
    }

    // the arena, the record attribute and the storage buffers for program `which` (bound)
    void setup_inputs(U32 which)
    {
        // the record attribute goes into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        if (sRiggedPass)
        {   // the skinned branch of pbropaqueV.glsl projects with the projection_matrix uniform (the static one only
            // reads the records' matrices). Only gGL's sync sets it, which every stock draw goes through and a
            // multi-draw doesn't: left unset it is all zeros and every rigged vertex collapses to one point.
            gGL.syncMatrices();
        }
        if (sReplayPass >= 0)
        {   // FSRenderGPUOcclusionCulling phase 2: the copies of this pass's inputs
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATRIX_BINDING, FSGPUOcclusion::replayMatrices(sReplayPass));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATERIAL_BINDING, FSGPUOcclusion::replayMaterials(sReplayPass));
        }
        else
        {
            sMatrices.bindBase(FS_GDD_PBR_MATRIX_BINDING);
            sMaterialBuffer.bindBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATERIAL_BINDING);
        }
        if (sRiggedPass)
        {   // the palette buffer whole; the records hold the offsets
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_SKIN_BINDING, FSSkinPaletteBuffer::glName());
        }
        if (sIndirectOverride)
        {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectOverride);
        }
        else
        {
            sIndirectBuffer.bind(GL_DRAW_INDIRECT_BUFFER);
        }
        const GLuint record = (GLuint)sRecordAttrib[which];
        if (sReplayPass >= 0)
        {
            glBindBuffer(GL_ARRAY_BUFFER, FSGPUOcclusion::replayRecords(sReplayPass));
        }
        else
        {
            sRecordBuffer.bind(GL_ARRAY_BUFFER);
        }
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), sReplayPass >= 0 ? (const void*)0 : sRecordBuffer.at(0));
        glVertexAttribDivisor(record, 1);
        sPassArena->bindForDraw(FS_GDD_PBR_ATTRIBS | (sRiggedPass ? LLVertexBuffer::MAP_WEIGHT4 : 0));
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 which)
    {
        const GLuint record = (GLuint)sRecordAttrib[which];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATRIX_BINDING, 0);
        if (sRiggedPass)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_SKIN_BINDING, 0);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_PBR_MATERIAL_BINDING, 0);  // also unbinds the generic binding
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    }

    // Where the multi-draws find the command `offset` bytes in
    const void* indirect_at(size_t offset)
    {
        return sIndirectOverride ? (const void*)offset : sIndirectBuffer.at(offset);
    }

    // FSRenderGPUOcclusionCulling: phase 1 for the pass just uploaded; the multi-draws then read its culled
    // commands. The pass's occlusion index, or -1 when it isn't culled.
    S32 cull_phase_one()
    {
        sCullEntries.resize(sCommandParams.size());
        for (size_t c = 0; c < sCommandParams.size(); ++c)
        {
            FSGPUOcclusion::makeEntry(*sCommandParams[c], sCullEntries[c]);
        }
        sRanges.clear();
        for (const Op& op : sOps)
        {
            if (op.mKind != OP_STOCK && op.mCount)
            {
                sRanges.push_back({ op.mFirst, op.mCount });
            }
        }
        FSGPUOcclusion::PassInputs inputs;
        inputs.mRanges = sRanges.data();
        inputs.mRangeCount = (U32)sRanges.size();
        inputs.mKind = sRiggedPass ? FSGPUOcclusion::KIND_RIGGED : FSGPUOcclusion::KIND_PBR;
        inputs.mCommands = sIndirectBuffer.name();
        inputs.mCommandsOffset = sIndirectBuffer.offset();
        inputs.mCommandCount = (U32)sCommands.size();
        inputs.mRecords = sRecordBuffer.name();
        inputs.mRecordsOffset = sRecordBuffer.offset();
        inputs.mRecordBytes = sRecords.size() * sizeof(Record);
        inputs.mRecordStride = sizeof(Record) / sizeof(U32);
        inputs.mMatrixField = 0;    // Record::mMatrix: the modelview, the modelview projection after it
        inputs.mMVPOffset = 1;
        inputs.mMatrices = sMatrices.buffer().name();
        inputs.mMatricesOffset = sMatrices.buffer().offset();
        inputs.mMatrixBytes = sMatrices.size() * sizeof(glm::mat4);
        inputs.mMaterials = sMaterialBuffer.name();
        inputs.mMaterialsOffset = sMaterialBuffer.offset();
        inputs.mMaterialBytes = sMaterials.size() * sizeof(Material);
        const S32 pass = FSGPUOcclusion::cullPhaseOne(inputs, sCullEntries);
        sIndirectOverride = pass >= 0 ? FSGPUOcclusion::phaseOneCommands(pass) : 0;
        sCullPass = sIndirectOverride ? pass : -1;
        return pass;
    }

    // One multi-draw op, the range-th of the pass's: FSRenderGPUOcclusionCulling packs a culled pass's ranges
    void multi_draw(const Op& op, U32 range)
    {
        const S32 occ = sReplayPass >= 0 ? sReplayPass : sCullPass;
        if (sIndirectOverride && occ >= 0 && op.mCount)
        {
            FSGPUOcclusion::drawRange(occ, sReplayPass >= 0, range, op.mFirst, op.mCount);
            return;
        }
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, indirect_at((size_t)op.mFirst * sizeof(FSGDDDrawCommand)),
                                    (GLsizei)op.mCount, 0);
    }

    // One batch the stock way: pushGLTFBatch, or for a rigged pass pushRiggedGLTFBatch (its palette's range
    // first; build left only batches that have one). The pass's stock program must be bound.
    void push_stock(LLDrawInfo& params, LLFetchedGLTFMaterial*& last_mat, LLViewerTexture*& last_tex)
    {
        if (sRiggedPass && !LLRenderPass::uploadMatrixPalette(params.mAvatar, params.mSkinInfo.get()))
        {
            return;
        }
        LLRenderPass::pushGLTFBatch(params, last_mat, last_tex);
    }

    // Rigged pass: every palette it draws with into the palette buffer, and the offsets into the records
    bool assign_palettes()
    {
        if (!FSSkinPaletteBuffer::place(sCommandParams, sStockRigged, sPaletteOffsets, sPaletteGeneration) ||
            sPaletteOffsets.size() != sRecords.size())
        {
            return false;
        }
        for (size_t c = 0; c < sRecords.size(); ++c)
        {
            sRecords[c].mSkin = (sRecords[c].mSkin & FS_GDD_SKIN_EXCLUDE_BIT) | sPaletteOffsets[c];
        }
        return true;
    }

    // Draws the ops. The stock program is bound on entry and on return. False if a GPU-driven program
    // couldn't be bound before anything was drawn (then nothing was). cull: FSRenderGPUOcclusionCulling phase 1
    // for this pass (occ: its occlusion index, or -1 when it wasn't culled).
    bool execute(U32& calls, U32& switches, bool cull, S32& occ)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr execute");
        LL_PROFILE_GPU_ZONE("gdd pbr");
        // the rigged pass apart: zone "gbuffer rigged" of the FSGPUTimings log, inside the pass's zone
        std::optional<LLFSGPUZone::Scope> rigged_zone;
        if (sRiggedPass)
        {
            rigged_zone.emplace("gbuffer rigged");
        }

        sIndirectBuffer.upload(GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(FSGDDDrawCommand));
        sRecordBuffer.upload(GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        sMatrices.upload();
        sMaterialBuffer.upload(GL_SHADER_STORAGE_BUFFER, sMaterials.data(), sMaterials.size() * sizeof(Material));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        LLVertexBuffer::sGLRenderBuffer = 0;    // or the next setBuffer of the buffer it named skips binding it

        sStockRanges.clear();
        occ = cull ? cull_phase_one() : -1;
        struct ResetOverride    // the next pass reads its own commands again, however this one ends
        {
            ~ResetOverride()
            {
                sIndirectOverride = 0;
                sCullPass = -1;
            }
        } reset_override;

        LLGLSLShader* stock = sStockProgram;
        const U32 base = sRiggedPass ? FS_GDD_PBR_RIGGED : 0;
        S32 current = -1;                       // GPU-driven program bound (program()), -1 stock
        bool drew = false;
        LLFetchedGLTFMaterial* stock_mat = nullptr;     // pushGLTFBatch's material cache
        LLViewerTexture* stock_tex = nullptr;
        LLFetchedGLTFMaterial* bound_mat = nullptr;     // last material bound for gFSGDDPBRProgram
        LLViewerTexture* bound_tex = nullptr;
        U32 next_range = 0;                     // the pass's multi-draw ops so far (multi_draw)

        for (const Op& op : sOps)
        {
            const U32 range = op.mKind != OP_STOCK && op.mCount ? next_range++ : 0;
            if (op.mKind == OP_STOCK)
            {
                if (current >= 0)
                {
                    finish_inputs(current);
                    stock->bind();
                    current = -1;
                    stock_mat = nullptr;    // the GPU-driven program may have bound other textures on the units
                    ++switches;
                }
                push_stock(*op.mParams, stock_mat, stock_tex);
                drew = true;
                continue;
            }

            if (sRiggedPass && FSSkinPaletteBuffer::generation() != sPaletteGeneration)
            {   // the palette buffer was replaced after the offsets were taken (not expected: place() put every
                // palette of the pass in it); the run the stock way, which binds each palette again
                LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenRigged: the palette buffer was replaced mid PBR pass" << LL_ENDL;
                if (op.mCount)
                {
                    mark_stock(range);
                }
                if (current >= 0)
                {
                    finish_inputs(current);
                    stock->bind();
                    current = -1;
                    stock_mat = nullptr;
                    ++switches;
                }
                for (U32 c = op.mFirst; c < op.mFirst + op.mCount; ++c)
                {
                    push_stock(*sCommandParams[c], stock_mat, stock_tex);
                }
                drew = true;
                continue;
            }

            const S32 which = (S32)(base + (op.mKind == OP_BINDLESS ? 1 : 0));
            if (current != which)
            {
                if (current >= 0)
                {
                    finish_inputs(current);
                }
                if (!bind_program(which))
                {
                    stock->bind();
                    if (!drew)
                    {
                        return false;
                    }
                    // not expected (fsGDDDrawPBR bound both programs once already); the rest of the pass goes
                    // the stock way, in order
                    LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenPBR: a program failed to bind mid pass" << LL_ENDL;
                    stock_mat = nullptr;
                    U32 rest_range = op.mCount ? range : next_range;   // this op's range, or the next one
                    for (size_t i = &op - sOps.data(); i < sOps.size(); ++i)
                    {
                        const Op& rest = sOps[i];
                        if (rest.mKind == OP_STOCK)
                        {
                            push_stock(*rest.mParams, stock_mat, stock_tex);
                        }
                        else
                        {
                            if (rest.mCount)
                            {
                                mark_stock(rest_range++);
                            }
                            for (U32 c = rest.mFirst; c < rest.mFirst + rest.mCount; ++c)
                            {
                                push_stock(*sCommandParams[c], stock_mat, stock_tex);
                            }
                        }
                    }
                    return true;
                }
                if (current < 0)
                {
                    ++switches;
                }
                current = which;
                setup_inputs(which);
                bound_mat = nullptr;
            }
            if (op.mKind == OP_BOUND && (op.mMat != bound_mat || op.mTex != bound_tex))
            {
                op.mMat->bind(op.mTex);
                bound_mat = op.mMat;
                bound_tex = op.mTex;
            }
            LLGLDisable cull_face(op.mDoubleSided ? GL_CULL_FACE : 0);
            multi_draw(op, range);
            ++calls;
            drew = true;
        }
        if (current >= 0)
        {
            finish_inputs(current);
            stock->bind();
        }
        STOP_GLERROR;
        return true;
    }

    // FSPerfGPUDrivenPBRVerify: the batches drawn from the arena again, the stock way, colour and depth writes
    // off, depth test GL_LESS against what the multi-draws left. The stock program must be bound.
    void verify_against_stock()
    {
        if (sCommandParams.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr verify");

        static LLFrameTimer error_timer;
        fsGDDCheckGLError(error_timer, "FSPerfGPUDrivenPBRVerify", "PBR");

        // arena copies against their sources, a couple per pass
        sStatMismatches += fsGDDVerifyArenaCopies(*sPassArena, sCommandParams,
                                                  [](LLDrawInfo* params) { return params->mVertexBuffer.get(); },
                                                  "FSPerfGPUDrivenPBRVerify");

        const GLuint samples = fsGDDNearerSamples(sQuery, false, [&]()
        {
            LLFetchedGLTFMaterial* last_mat = nullptr;
            LLViewerTexture* last_tex = nullptr;
            for (LLDrawInfo* params : sCommandParams)
            {
                push_stock(*params, last_mat, last_tex);
            }
        });
        if (samples)
        {
            ++sStatMismatches;
            static LLFrameTimer warn_timer;
            if (fsGDDWarnDue(warn_timer))
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenPBRVerify: " << samples << " samples of " << sCommandParams.size()
                                      << " stock PBR batches are nearer than the multi-draw depth; "
                                      << sStatMismatches << " mismatches so far" << LL_ENDL;
            }
        }
    }

    // FSRenderGPUOcclusionCulling phase 2 of a pass (FSGPUOcclusion::flushPhaseTwo): its multi-draws again,
    // reading the phase 2 commands and the copies of its inputs. Stock ops were drawn in phase 1.
    void replay_phase_two(S32 occ, U32 user)
    {
        if (user >= sReplayUsed || sReplayFrame != LLFrameTimer::getFrameCount())
        {
            return;
        }
        Replay& replay = sReplays[user];
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr phase 2");
        LL_PROFILE_GPU_ZONE("gdd pbr phase 2");
        if (replay.mRigged && FSSkinPaletteBuffer::generation() != replay.mGeneration &&
            !FSGPUOcclusion::refreshPalettes(occ, replay.mParams, replay.mRecords, sizeof(Record) / sizeof(U32), 3,
                                             replay.mGeneration))
        {   // not expected: place() found no buffer, or it never held still
            LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: the joint palettes of a rigged PBR pass could not be "
                                       << "put in the palette buffer again for its phase 2" << LL_ENDL;
            return;
        }
        sRiggedPass = replay.mRigged;
        std::optional<LLFSGPUZone::Scope> rigged_zone;
        if (sRiggedPass)
        {
            rigged_zone.emplace("gbuffer rigged");
        }
        sPassArena = sRiggedPass ? &sRiggedArena : &sArena;
        const U32 base = sRiggedPass ? FS_GDD_PBR_RIGGED : 0;
        sReplayPass = occ;
        sIndirectOverride = FSGPUOcclusion::replayCommands(occ);
        LLVertexBuffer::unbind();
        LLGLDepthTest depth(GL_TRUE, GL_TRUE);
        LLGLDisable blend(GL_BLEND);

        S32 current = -1;
        LLFetchedGLTFMaterial* bound_mat = nullptr;
        LLViewerTexture* bound_tex = nullptr;
        U32 calls = 0;
        U32 next_range = 0;
        for (const Op& op : replay.mOps)
        {
            const U32 range = next_range++;     // replay.mOps are the pass's multi-draw ops, in order
            if (!op.mCount)
            {   // drawn the stock way in phase 1 (keep_for_phase_two)
                continue;
            }
            const S32 which = (S32)(base + (op.mKind == OP_BINDLESS ? 1 : 0));
            if (current != which)
            {
                if (current >= 0)
                {
                    finish_inputs(current);
                    current = -1;
                }
                program(which).mUniformsDirty = true;
                if (!bind_program(which))
                {   // not expected: it drew in phase 1 this frame
                    LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: a PBR program failed to bind in phase 2" << LL_ENDL;
                    continue;
                }
                current = which;
                setup_inputs(which);
                bound_mat = nullptr;
            }
            if (op.mKind == OP_BOUND && (op.mMat != bound_mat || op.mTex != bound_tex))
            {
                op.mMat->bind(op.mTex);
                bound_mat = op.mMat;
                bound_tex = op.mTex;
            }
            LLGLDisable cull_face(op.mDoubleSided ? GL_CULL_FACE : 0);
            multi_draw(op, range);
            ++calls;
        }
        if (current >= 0)
        {
            finish_inputs(current);
        }
        sReplayPass = -1;
        sIndirectOverride = 0;
        sStatCalls += calls;
        STOP_GLERROR;
    }

    // Keeps the ops of a pass phase 1 culled, for its phase 2
    void keep_for_phase_two(S32 occ)
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        if (sReplayFrame != frame)
        {
            sReplayFrame = frame;
            sReplayUsed = 0;
        }
        if (sReplayUsed >= sReplays.size())
        {
            sReplays.resize(sReplayUsed + 1);
        }
        Replay& replay = sReplays[sReplayUsed];
        replay.mOps.clear();
        U32 range = 0;
        for (const Op& op : sOps)
        {
            if (op.mKind != OP_STOCK && op.mCount)
            {   // one drawn the stock way stays, with no commands, so the ranges keep their numbers
                replay.mOps.push_back(op);
                if (range < sStockRanges.size() && sStockRanges[range])
                {
                    replay.mOps.back().mCount = 0;
                }
                ++range;
            }
        }
        replay.mRigged = sRiggedPass;
        replay.mParams.clear();
        replay.mRecords.clear();
        if (sRiggedPass)
        {   // to put the palettes in the buffer again, should it be replaced before phase 2
            replay.mGeneration = sPaletteGeneration;
            replay.mParams = sCommandParams;
            replay.mRecords.resize(sRecords.size() * (sizeof(Record) / sizeof(U32)));
            memcpy(replay.mRecords.data(), sRecords.data(), sRecords.size() * sizeof(Record));
        }
        FSGPUOcclusion::addReplay(occ, &replay_phase_two, sReplayUsed);
        ++sReplayUsed;
    }

    void log_stats()
    {
        static LLFrameTimer timer;
        if (timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        const std::string reasons = fsGDDCountList(sStatReasons, R_COUNT, reason_name);
        U32 handle_textures = 0;
        {
            std::lock_guard<std::mutex> lock(sHandleMutex);
            handle_textures = (U32)sHandles.size();
        }
        // texture lookups without a handle by cause, across every GDD pass (PBR, materials, legacy, alpha, shadows)
        static const char* no_handle_names[NH_COUNT] = { "not a fetched texture", "not loaded", "not 2D", "options pending",
                                                         "moved by a partial update", "compressed", "driver gave none",
                                                         "being made" };
        const std::string no_handle = fsGDDCountList(sStatNoHandle, NH_COUNT, [](U32 r) { return no_handle_names[r]; });
        // handles and textures held until the GPU finished the frames that may sample them (FSGLRetire)
        const FSGLRetire::Stats retire = FSGLRetire::takeStats();
        LL_INFOS("FSGPUDrivenPBR") << "GPU-driven PBR, last 10 s: " << sStatPasses << " passes, " << sStatBatches
                                   << " batches (" << sStatBindless << " bindless, " << sStatBound
                                   << " bound textures) in " << sStatCalls << " multi-draw calls, "
                                   << sStatSwitches << " program switches; stock path or bound textures for: "
                                   << reasons << "; arena "
                                   << fsGDDArenaSummary(sArena) << "; rigged arena "
                                   << fsGDDArenaSummary(sRiggedArena) << "; bindless "
                                   << handle_textures << " textures, " << sResident << " resident handles, "
                                   << sStatHandlesMade << " made, " << sStatHandleFailures << " failed, "
                                   << sStatUnlocks << " dropped for texture changes, " << sStatPreviousHandle
                                   << " lookups served by the previous texture's handle; retired behind GPU fences: "
                                   << retire.mHandlesRetired << " handles, " << retire.mTexturesRetired << " textures, "
                                   << retire.mWaiting << " waiting, " << retire.mReleasedEarly
                                   << " released early; lookups without a handle: "
                                   << no_handle << "; " << sStatMismatches
                                   << " verify mismatches" << LL_ENDL;
        sStatPasses = sStatBatches = sStatBindless = sStatBound = sStatCalls = sStatSwitches = 0;
        sStatHandlesMade = sStatHandleFailures = sStatUnlocks = sStatPreviousHandle = 0;
        sArena.clearCounts();
        sRiggedArena.clearCounts();
    }
}

namespace
{
    // fsGDDDrawPBR (rigged false) and fsGDDDrawRiggedPBR (rigged true)
    bool draw_pbr(U32 type, bool rigged);
}

bool fsGDDDrawPBR(U32 type)
{
    // before any early return, so idle handles go non resident and away also while no PBR batch (or no
    // bindless one) is drawn; cheap, it does something every 120 frames
    sweep_handles(LLFrameTimer::getFrameCount());
    return draw_pbr(type, false);
}

bool fsGDDDrawRiggedPBR(U32 type)
{
    if (!fsGDDRiggedEnabled())
    {
        return false;
    }
    return draw_pbr(type, true);
}

namespace
{
bool draw_pbr(U32 type, bool rigged)
{
    // the stock program the pool bound: gDeferredPBROpaqueProgram, or for the rigged pass its skinned variant
    LLGLSLShader* stock = rigged ? gDeferredPBROpaqueProgram.mRiggedVariant : &gDeferredPBROpaqueProgram;
    const U32 base = rigged ? FS_GDD_PBR_RIGGED : 0;
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenPBR", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !stock || (rigged && stock == &gDeferredPBROpaqueProgram) ||
        !program_usable(base) || LLGLSLShader::sCurBoundShaderPtr != stock || LLPipeline::sShadowRender)
    {
        return false;
    }
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    if (begin == end)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    static LLCachedControl<bool> want_bindless(gSavedSettings, "FSPerfGPUDrivenPBRBindless", false);
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenPBRVerify", false);
    static LLCachedControl<bool> stats(gSavedSettings, "FSPerfGPUDrivenPBRStats", false);
    static LLCachedControl<bool> group_runs(gSavedSettings, "FSPerfGPUDrivenPBRGroupRuns", false);

    // The first bind of each program finds its inputs; do that now, so a failure leaves the whole pass
    // to the stock loop rather than part of it
    if (!sInputsReady[base])
    {
        const bool ready = bind_program(base);
        stock->bind();
        if (!ready)
        {
            return false;
        }
    }
    bool bindless = want_bindless && sBindlessTested > 0 && program_usable(base + 1);
    if (bindless && !sInputsReady[base + 1])
    {
        bindless = bind_program(base + 1);
        stock->bind();
    }

    // The environment uniforms (LLEnvironment marks the programs of the shader list dirty when they change,
    // and the stock program took them when the pool bound it); ours aren't in that list, so they take the
    // current values at their first bind in each pass
    program(base).mUniformsDirty = true;
    program(base + 1).mUniformsDirty = true;

    sRiggedPass = rigged;
    sStockProgram = stock;
    sPassArena = rigged ? &sRiggedArena : &sArena;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd pbr build");
        build(begin, end, bindless, group_runs);
    }
    if (rigged && !sCommands.empty() && !assign_palettes())
    {   // no buffer, or it never held still: the stock loop draws the pass
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenRigged: the joint palettes could not be put in the palette buffer; "
                                   << "a rigged PBR pass is drawn as before" << LL_ENDL;
        return false;
    }
    if (sCommands.empty())
    {   // nothing from the arena: the stock loop draws exactly the same
        if (verify || stats)
        {
            ++sStatPasses;
            log_stats();
        }
        return false;
    }

    U32 calls = 0;
    U32 switches = 0;
    // FSRenderGPUOcclusionCulling: the main camera's passes; not while verifying, which compares against
    // everything the stock path draws
    const bool cull = !verify && FSGPUOcclusion::passActive();
    S32 occ = -1;
    if (!execute(calls, switches, cull, occ))
    {
        return false;
    }
    if (occ >= 0)
    {
        keep_for_phase_two(occ);
    }

    if (verify)
    {
        verify_against_stock();
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;

    if (verify || stats)
    {
        ++sStatPasses;
        sStatCalls += calls;
        sStatSwitches += switches;
        log_stats();
    }
    return true;
}
} // namespace
