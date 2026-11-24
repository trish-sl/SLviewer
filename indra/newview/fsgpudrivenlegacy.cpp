/**
 * @file fsgpudrivenlegacy.cpp
 * @brief GPU-driven draws for the deferred legacy (non PBR) opaque passes (FSPerfGPUDrivenLegacy)
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

// The stock legacy opaque passes (LLRenderPass::pushBatch for PASS_SIMPLE and PASS_FULLBRIGHT, pushMaskBatches
// for PASS_ALPHA_MASK and PASS_FULLBRIGHT_ALPHA_MASK) cost, for every batch, a model matrix (modelview,
// modelview projection and normal matrix uniforms), up to 16 texture binds (the batch's texture list, which the
// indexed texture shaders pick from by the vertex's texture index) or one bind and a texture matrix, a minimum
// alpha uniform in the masked passes, a vertex buffer bind and a glDrawRangeElements.
//
// With FSPerfGPUDrivenLegacy on, the batches come out of a vertex arena instead (fsgddarena.h: a GPU side copy
// of the position, normal, texcoord0 and colour regions and the 16 bit indices of their vertex buffers; the
// texture index is the w of the position, so it comes along; normals are optional, fullbright buffers have
// none), and each batch is one 20 byte indirect draw command plus a record: which model matrices (in a storage
// buffer, the matrices the stock path would have computed, computed the same way), which texture matrix
// (identity, or the batch's animation when pushBatch would load it), and where its textures start in a second
// storage buffer: a header (the minimum alpha pushMaskBatches would set, the texture count) and the bindless
// handle of every texture pushBatch would bind, in unit order. diffuseV.glsl / fullbrightV.glsl built with
// FS_GDD_LEGACY pick the handle the vertex's texture index selects, exactly as diffuseLookup picks a unit; the
// fragment shaders are the stock ones with diffuseLookup and minimum_alpha replaced.
//
// The texture handles are those of FSPerfGPUDrivenPBRBindless (fsgpudrivenpbr.cpp: a sampler per texture
// state, dropped before LLImageGL changes a texture), so this needs bindless textures, which are only used on
// NVIDIA; without them the passes stay as they are, since binding textures is most of what this saves.
//
// The batches are drawn in the stock order: one glMultiDrawElementsIndirect per run of batches from the arena.
// A batch that can't come from it (rigged, empty, a texture missing or without a handle (not loaded, one or
// two channels, options pending, media), 32 bit indices, buffer mapped or lacking an attribute, arena full or
// the copy budget used up) is drawn by the stock code in its place.
//
// FSPerfGPUDrivenLegacyVerify: after the pass, the batches it drew from the arena are drawn again the stock way
// with colour and depth writes off and depth test GL_LESS under a GL_SAMPLES_PASSED query: a sample passes only
// where the stock geometry is nearer than what the multi-draws left, which is logged (only where the pass
// writes depth; the fullbright passes may not). It also reads back a couple of arena entries per pass and
// compares them with their vertex buffers, checks for GL errors, and logs statistics every 10 seconds
// (FSPerfGPUDrivenLegacyStats logs only the statistics).
//
// FSPerfGPUDrivenRigged (with FSPerfRiggedPaletteBuffer, fsskinpalettebuffer.h): the rigged passes of the same
// pools (PASS_SIMPLE_RIGGED, PASS_ALPHA_MASK_RIGGED, PASS_FULLBRIGHT_RIGGED, PASS_FULLBRIGHT_ALPHA_MASK_RIGGED:
// pushRiggedBatches / pushRiggedMaskBatches with the stock program's skinned variant bound) the same way, as
// in the legacy material passes (fsgpudrivenmaterials.cpp). The skinned programs are built from the stock
// skinned ones with FS_GDD_LEGACY and LLShaderFeatures::mFSGDDSkinning, which links the copy of
// objectSkinV.glsl built with FS_GDD_RIGGED. Their batches come from a second arena that also holds the joint
// weights, and each record names (its w, Record::mSkin) where the batch's palette starts in this frame's
// palette buffer, which the pass binds whole as a storage buffer: the palette the stock loop would bind, the
// same floats the stock programs read through FSSkinPalette, read by the same skinning code. The skinned
// branches of diffuseV.glsl and fullbrightV.glsl project with the projection_matrix uniform, which only
// gGL.syncMatrices sets: it runs before the multi-draws. A batch without avatar, skin or palette yet is
// skipped, as the stock loops skip it. All palettes of the pass go into the buffer before it draws
// (FSSkinPaletteBuffer::place), the stock fallbacks' too; should the buffer still be replaced mid pass, the
// multi-draws left are drawn the stock way.
//
// FSPerfGPUDrivenBuildCache (fsgddcommon.cpp): a non rigged pass given the same batches as in an earlier frame uses
// the commands, records and texture lists built then, from their own buffers, once the texture handles and arena
// copies they name are found to be the same; only its matrices are looked up again.
//
// FSRenderGPUOcclusionCulling (fsgpuocclusion.h), the simple and alpha mask passes of the main camera's G-buffer
// (the fullbright ones draw after it): as in the PBR pass (fsgpudrivenpbr.cpp), the pass's commands go through
// the phase 1 cull first and the multi-draws read the culled copy (sIndirectOverride), which keeps only the
// batches visible last frame. The pass's runs are kept, and once every opaque pool has drawn, replay_phase_two
// draws them again with the phase 2 commands (the batches found visible only then) from copies of the pass's
// records, matrices and texture lists (sReplayPass). Batches drawn the stock way are drawn in phase 1 only. The
// rigged simple and alpha mask passes too (FSPerfGPUDrivenRigged; their boxes come from their avatars): should
// the palette buffer be replaced before phase 2, their palettes are put in it again and the records' offsets
// rewritten (FSGPUOcclusion::refreshPalettes).

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenlegacy.h"
#include "fsreversez.h"

#include "fsgddarena.h"
#include "fsgddcommon.h"
#include "fsgpudrivenmaterials.h"
#include "fsgpudrivenpbr.h"
#include "fsgpuocclusion.h"
#include "fsskinpalettebuffer.h"

#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <cstring>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

LLGLSLShader gFSGDDDiffuseProgram;
LLGLSLShader gFSGDDDiffuseAlphaMaskProgram;
LLGLSLShader gFSGDDFullbrightProgram;
LLGLSLShader gFSGDDFullbrightAlphaMaskProgram;

namespace
{
    // the PBR pass uses the same bindings; each pass binds its buffers and clears them again
    constexpr GLuint FS_GDD_LEGACY_MATRIX_BINDING = 5;  // shader storage binding of FSGDDLegacyMatrices
    constexpr GLuint FS_GDD_LEGACY_TEXTURE_BINDING = 6; // and of FSGDDLegacyTextures
    constexpr GLuint FS_GDD_LEGACY_SKIN_BINDING = 7;    // and of FSGDDSkinPalettes (objectSkinV.glsl, rigged)
    // the attributes the arenas can feed; a program wanting any other is not used (the weights only the rigged
    // arena has; the rigged programs read them, the others don't)
    constexpr U32 FS_GDD_LEGACY_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0 |
                                          LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_WEIGHT4;

    // everything diffuseV.glsl reads (fullbrightV.glsl: all but the normal, which fullbright buffers lack), an
    // eighth of FSPerfGPUDrivenLegacyArenaMB for indices
    FSGDDArena sArena("FSPerfGPUDrivenLegacy arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                        LLVertexBuffer::TYPE_COLOR },
                      "FSPerfGPUDrivenLegacyArenaMB", 512, 8, LLVertexBuffer::MAP_NORMAL);
    // FSPerfGPUDrivenRigged: the same plus the joint weights, for the rigged passes, so rigged buffers neither
    // crowd the static ones out nor make every vertex of the first arena carry weights. The same size limit, its own.
    // Most rigged meshes are PBR or have materials now, so this one usually holds a few MB: with
    // FSAutoSizeResources and no peak remembered it starts at an eighth of the usual first size (its 66 MB
    // first allocation held 1 MB in a busy scene).
    FSGDDArena sRiggedArena("FSPerfGPUDrivenRigged legacy arena",
                            { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                              LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_WEIGHT4 },
                            "FSPerfGPUDrivenLegacyArenaMB", 512, 8, LLVertexBuffer::MAP_NORMAL, 8);

    struct Record       // fs_gdd_draw
    {
        U32 mMatrix;    // modelview; modelview projection and normal matrix follow
        U32 mTexMatrix;
        U32 mTextures;  // first entry in FSGDDLegacyTextures: the header, then the handles
        U32 mSkin;      // rigged: the palette's first vec4 in the palette buffer (FSSkinPaletteBuffer::place)
    };

    static_assert(sizeof(FSGDDTexEntry) == 8, "FSGDDLegacyTextures layout");

    struct Op
    {
        bool mStock;            // pool.pushBatch(*mParams)
        U32 mFirst;             // first command
        U32 mCount;             // commands
        LLDrawInfo* mParams;
    };

    enum EPass
    {
        P_SIMPLE = 0,
        P_ALPHA_MASK,
        P_FULLBRIGHT,
        P_FULLBRIGHT_ALPHA_MASK,
        P_COUNT
    };

    struct PassInfo
    {
        U32 mType;
        LLGLSLShader* mStock;
        LLGLSLShader* mProgram;
        const char* mName;
        bool mMask;             // pushMaskBatches: minimum alpha per batch
        bool mNormals;          // the program reads normals
        U32 mRiggedType;        // FSPerfGPUDrivenRigged: the rigged pass (the stock program's skinned variant)
        const char* mRiggedName;
    };

    const PassInfo sPasses[P_COUNT] =
    {
        { LLRenderPass::PASS_SIMPLE, &gDeferredDiffuseProgram, &gFSGDDDiffuseProgram,
          "FS GPU-Driven Diffuse Shader", false, true,
          LLRenderPass::PASS_SIMPLE_RIGGED, "FS GPU-Driven Skinned Diffuse Shader" },
        { LLRenderPass::PASS_ALPHA_MASK, &gDeferredDiffuseAlphaMaskProgram, &gFSGDDDiffuseAlphaMaskProgram,
          "FS GPU-Driven Diffuse Alpha Mask Shader", true, true,
          LLRenderPass::PASS_ALPHA_MASK_RIGGED, "FS GPU-Driven Skinned Diffuse Alpha Mask Shader" },
        { LLRenderPass::PASS_FULLBRIGHT, &gDeferredFullbrightProgram, &gFSGDDFullbrightProgram,
          "FS GPU-Driven Fullbright Shader", false, false,
          LLRenderPass::PASS_FULLBRIGHT_RIGGED, "FS GPU-Driven Skinned Fullbright Shader" },
        { LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, &gDeferredFullbrightAlphaMaskProgram, &gFSGDDFullbrightAlphaMaskProgram,
          "FS GPU-Driven Fullbright Alpha Mask Shader", true, false,
          LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED, "FS GPU-Driven Skinned Fullbright Alpha Mask Shader" },
    };

    // A program slot: the pass index, plus P_COUNT for its rigged program
    constexpr U32 SLOT_COUNT = P_COUNT * 2;
    LLGLSLShader sRiggedPrograms[P_COUNT];      // FSPerfGPUDrivenRigged, by pass

    bool slot_rigged(U32 slot)
    {
        return slot >= P_COUNT;
    }

    const PassInfo& pass_of(U32 slot)
    {
        return sPasses[slot % P_COUNT];
    }

    LLGLSLShader& program_of(U32 slot)
    {
        return slot_rigged(slot) ? sRiggedPrograms[slot - P_COUNT] : *sPasses[slot].mProgram;
    }

    // the stock program of the slot's pass: the pass's own, or its skinned variant (nullptr if it has none)
    LLGLSLShader* stock_of(U32 slot)
    {
        LLGLSLShader* stock = pass_of(slot).mStock;
        if (!slot_rigged(slot))
        {
            return stock;
        }
        return stock->mRiggedVariant != stock ? stock->mRiggedVariant : nullptr;
    }

    // why a batch went the stock way
    enum EReason
    {
        R_RIGGED = 0,
        R_EMPTY,
        R_NO_TEXTURE,
        R_HANDLE,
        R_NORMALS,
        R_RANGE,
        R_FADING,                       // fading in (FSRenderFadeIn)
        R_ARENA,                        // + FSGDDArena::EFail
        R_COUNT = R_ARENA + FSGDDArena::FAIL_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "rigged", "empty", "no texture", "texture without handle", "no normals", "index range",
                                       "fading in" };
        return reason < R_ARENA ? names[reason] : FSGDDArena::failName(reason - R_ARENA);
    }

    FSGDDBuffer sIndirectBuffer;
    FSGDDBuffer sRecordBuffer;
    FSGDDBuffer sTextureBuffer;
    GLuint sQuery = 0;

    // The buffers the pass being drawn reads (the ones above, or with FSPerfGPUDrivenBuildCache a kept pass's), and
    // which of them execute uploads to
    enum EUpload
    {
        UPLOAD_COMMANDS = 1,
        UPLOAD_RECORDS = 2,
        UPLOAD_TEXTURES = 4,
        UPLOAD_ALL = 7
    };
    FSGDDBuffer* sCurIndirect = &sIndirectBuffer;
    FSGDDBuffer* sCurRecords = &sRecordBuffer;
    FSGDDBuffer* sCurTextures = &sTextureBuffer;
    U32 sUploads = UPLOAD_ALL;
    GLint sRecordAttrib[SLOT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1 };  // location of fs_gdd_draw, by slot
    S32 sInputsReady[SLOT_COUNT] = {};                  // bind_inputs done: 0 not yet, 1 yes, -1 failed
    // the pass being drawn: rigged, its arena
    bool sRiggedPass = false;
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
    std::vector<FSGDDTexEntry> sTextures;
    std::vector<LLDrawInfo*> sCommandParams;    // per command: its batch (verify, and the stock fallback)
    // consecutive batches mostly share their matrices: the last answers skip the map lookups
    U32 sPassSerial = 1;    // the pass being built; textures remember their handle by it (mFSGDDPass), also
                            // for FSPerfGPUDrivenMaterials (fsGDDBeginTexturePass)
    std::vector<FSGDDTextureUse>* sRecordedTextures = nullptr;     // fsGDDRecordTextureUses

    // FSPerfGPUDrivenBuildCache: the kept passes, and by their slot what the build made besides the buffers
    constexpr U32 FS_GDD_LEGACY_KEPT = 16;
    FSGDDBuildCache sCache(FS_GDD_BUILDER_LEGACY, FS_GDD_LEGACY_KEPT);
    struct Kept
    {
        std::vector<Op> mOps;
        std::vector<FSGDDDrawCommand> mCommands;
        std::vector<Record> mRecords;
        std::vector<FSGDDTexEntry> mTextures;
        std::vector<LLDrawInfo*> mParams;
    };
    std::vector<Kept> sKept(FS_GDD_LEGACY_KEPT);
    // what a build that may be kept looked up, and whether it may be
    std::vector<FSGDDTextureUse> sTextureUses;
    std::vector<FSGDDArenaUse> sArenaUses;
    std::vector<FSGDDMatrixUse> sMatrixUses;
    bool sKeepable = false;
    std::vector<U32> sRemap;

    // FSRenderGPUOcclusionCulling: the indirect buffer the multi-draws read instead of sIndirectBuffer (at offset
    // 0; 0 for none), the occlusion pass whose input copies setup_inputs binds (phase 2; -1 for none), the cull
    // entries, and per frame the program slot and runs of each culled pass for its phase 2
    GLuint sIndirectOverride = 0;
    S32 sReplayPass = -1;
    std::vector<FSGPUOcclusion::CullEntry> sCullEntries;
    S32 sCullPass = -1;     // phase 1: the occlusion index of the pass being drawn (-1 for none)
    std::vector<FSGPUOcclusion::Range> sRanges;    // its multi-draw ops' commands, in order
    struct Replay
    {
        U32 mSlot = 0;
        std::vector<Op> mOps;   // the runs (no stock ops)
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
    U32 sReplayUsed = 0;        // sReplays of this frame

    // stats, every 10 seconds with FSPerfGPUDrivenLegacyVerify or FSPerfGPUDrivenLegacyStats
    U64 sStatBatches = 0;
    U64 sStatDrawn = 0;
    U64 sStatCalls = 0;
    U64 sStatSwitches = 0;
    U64 sStatPasses = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;
    U64 sStatUnverified = 0;    // passes without depth writes, where the occlusion check can't run
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    bool create_program(U32 slot)
    {
        const LLGLSLShader& stock = *stock_of(slot);
        LLGLSLShader& shader = program_of(slot);
        shader.mName = slot_rigged(slot) ? pass_of(slot).mRiggedName : pass_of(slot).mName;
        shader.mFeatures = stock.mFeatures;
        // no indexed texture units: the vertex shader reads texture_index itself and diffuseLookup samples a handle
        shader.mFeatures.mIndexedTextureChannels = 0;
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_ALPHA_MASK, HAS_EMISSIVE, ...), then ours
        shader.addPermutation("FS_GDD_LEGACY", "1");
        // rigged (HAS_SKIN among the stock defines): the skinning object that reads the palettes from the storage buffer
        shader.mFeatures.mFSGDDSkinning = slot_rigged(slot);
        return shader.createShader();
    }

    // the storage block bindings, where the driver put the per command record, and whether the arena can feed
    // every attribute the program reads
    bool bind_inputs(LLGLSLShader& shader, bool rigged, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDLegacyMatrices");
        const GLuint textures = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDLegacyTextures");
        const GLuint palettes = rigged ? glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDSkinPalettes")
                                       : GL_INVALID_INDEX;
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || textures == GL_INVALID_INDEX || (rigged && palettes == GL_INVALID_INDEX) ||
            record_attrib < 0 || (shader.mAttributeMask & ~FS_GDD_LEGACY_ATTRIBS) ||
            !(shader.mAttributeMask & LLVertexBuffer::MAP_TEXTURE_INDEX) ||
            (rigged != ((shader.mAttributeMask & LLVertexBuffer::MAP_WEIGHT4) != 0)))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_LEGACY_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, textures, FS_GDD_LEGACY_TEXTURE_BINDING);
        if (rigged)
        {
            glShaderStorageBlockBinding(shader.mProgramObject, palettes, FS_GDD_LEGACY_SKIN_BINDING);
        }
        return true;
    }

    // Binds the slot's GPU-driven program; the first time, also finds its inputs. False (for good) when they
    // aren't there.
    bool bind_program(U32 slot)
    {
        LLGLSLShader& shader = program_of(slot);
        shader.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &shader)
        {
            return false;
        }
        if (!sInputsReady[slot])
        {
            sInputsReady[slot] = bind_inputs(shader, slot_rigged(slot), sRecordAttrib[slot]) ? 1 : -1;
            if (sInputsReady[slot] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenLegacy: " << shader.mName << " lacks its storage blocks or draw "
                                      << "record attribute, or reads an attribute the arena doesn't hold; its batches "
                                      << "are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[slot] > 0;
    }

    bool program_usable(U32 slot)
    {
        return program_of(slot).isComplete() && sInputsReady[slot] >= 0;
    }
}

void fsGDDLegacyUnloadShaders()
{
    for (U32 slot = 0; slot < SLOT_COUNT; ++slot)
    {
        program_of(slot).unload();
        sInputsReady[slot] = 0;
        sRecordAttrib[slot] = -1;
    }
    fsGDDLegacyReleaseGL();
}

void fsGDDLegacyLoadShaders()
{
    fsGDDLegacyUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenLegacy", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenLegacy: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; legacy batches are drawn as before" << LL_ENDL;
        return;
    }
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenLegacy: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; legacy batches are drawn as before" << LL_ENDL;
        return;
    }
    // without bindless textures every batch would still bind its textures, which is most of what this saves
    if (!fsGDDBindlessSetup())
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenLegacy: needs bindless textures (NVIDIA GPUs only); "
                                       << "legacy batches are drawn as before" << LL_ENDL;
        return;
    }

    // FSPerfGPUDrivenRigged: the skinned programs too (optional: without them the rigged passes stay stock)
    const bool rigged = fsGDDRiggedLoadable(vs_blocks);
    for (U32 slot = 0; slot < SLOT_COUNT; ++slot)
    {
        if (slot_rigged(slot) && !rigged)
        {
            break;
        }
        const LLGLSLShader* stock = stock_of(slot);
        if (!stock || stock->mShaderFiles.empty())
        {
            continue;
        }
        if (!create_program(slot))
        {
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenLegacy: " << program_of(slot).mName << " failed to build, "
                                      << "its batches are drawn as before" << LL_ENDL;
            program_of(slot).unload();
        }
    }
}

void fsGDDLegacyReleaseGL()
{
    sCache.release();
    for (Kept& kept : sKept)
    {
        kept = Kept();
    }
    sArena.releaseGL();
    sRiggedArena.releaseGL();
    sIndirectBuffer.release();
    sRecordBuffer.release();
    sTextureBuffer.release();
    sMatrices.release();
    fsGDDDeleteQuery(sQuery);
}

//-----------------------------------------------------------------------------
// The per pass texture handle cache

void fsGDDBeginTexturePass()
{
    if (++sPassSerial == 0)
    {
        sPassSerial = 1; // 0 is what a texture starts with
    }
}

U64 fsGDDPassTextureHandle(LLViewerTexture* tex, U32 frame)
{
    if (tex->mFSGDDPass != sPassSerial)
    {
        tex->mFSGDDPass = sPassSerial;
        tex->mFSGDDHandle = fsGDDTextureHandle(tex, frame);
        if (tex->mFSGDDHandle)
        {
            tex->setActive();
        }
        if (sRecordedTextures)
        {
            sRecordedTextures->push_back({ tex, tex->mFSGDDHandle });
        }
    }
    return tex->mFSGDDHandle;
}

void fsGDDRecordTextureUses(std::vector<FSGDDTextureUse>* uses)
{
    sRecordedTextures = uses;
}

//-----------------------------------------------------------------------------
// The pass

namespace
{
    U64 handle_of(LLViewerTexture* tex, U32 frame)
    {
        return fsGDDPassTextureHandle(tex, frame);
    }

    void push_handle(U64 handle)
    {
        sTextures.push_back({ (U32)(handle & 0xFFFFFFFFull), (U32)(handle >> 32) });
    }

    // FSPerfGPUDrivenBuildCache: the kept pass, if what it names is still where it was (the texture handles, the
    // arena copies); then its ops, commands, records and texture lists are the pass's, its buffers the ones drawn
    // from, and the matrix table holds its matrices. False, with nothing taken, otherwise.
    bool use_kept(FSGDDKeptPass& kept, FSGDDArena& arena, U32 frame, const glm::mat4& proj, const glm::mat4& view)
    {
        fsGDDBeginTexturePass();
        for (const FSGDDTextureUse& use : kept.mTextures)
        {
            if (fsGDDPassTextureHandle(use.mTex, frame) != use.mHandle)
            {
                return false;
            }
        }
        const size_t count = kept.mArena.size();
        for (size_t i = 0; i < count; ++i)
        {
            if (i + 8 < count)
            {
                arena.prefetchBuffer(kept.mArena[i + 8].mBuffer);
            }
            if (i + 4 < count)
            {
                arena.prefetchEntry(kept.mArena[i + 4].mBuffer);
            }
            const FSGDDArenaUse& use = kept.mArena[i];
            const FSGDDArenaEntry* entry = arena.find(use.mBuffer, frame);
            if (!entry || entry->mVertex != use.mVertex || entry->mIndex != use.mIndex)
            {
                return false;
            }
        }

        Kept& k = sKept[sCache.slotOf(&kept)];
        sOps = k.mOps;
        sCommands = k.mCommands;
        sRecords = k.mRecords;
        sTextures = k.mTextures;
        sCommandParams = k.mParams;
        sUploads = 0;
        sMatrices.begin(proj, view, true, true);
        if (!sMatrices.replay(kept.mMatrices, proj, view, sRemap))
        {   // the matrices are at other indices than the kept records say (FSPerfGPUDrivenSharedMatrices): the
            // records follow, kept so, and go up again
            for (Record& record : sRecords)
            {
                record.mMatrix = record.mMatrix < sRemap.size() ? sRemap[record.mMatrix] : record.mMatrix;
                record.mTexMatrix = record.mTexMatrix < sRemap.size() ? sRemap[record.mTexMatrix] : record.mTexMatrix;
            }
            k.mRecords = sRecords;
            sUploads = UPLOAD_RECORDS;
        }
        sCurIndirect = &kept.mCommandBuffer;
        sCurRecords = &kept.mRecordBuffer;
        sCurTextures = &kept.mDataBuffer;
        sStatBatches += kept.mBatches;
        sStatDrawn += sCommands.size();
        return true;
    }

    // FSPerfGPUDrivenBuildCache: keeps the pass just built, which then uploads to and draws from the kept buffers
    void keep(U64 key, U64 hash, LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end, U32 batches)
    {
        FSGDDKeptPass* kept = sCache.claim(key);
        if (!kept)
        {
            sCache.countUnkept();
            return;
        }
        kept->keepInput(key, hash, (const uintptr_t*)begin, (size_t)(end - begin), 1);
        kept->mTextures.swap(sTextureUses);
        kept->mArena.swap(sArenaUses);
        FSGDDMatrixTable::finishUses(sMatrixUses);
        kept->mMatrices.swap(sMatrixUses);
        kept->mBatches = batches;
        kept->mValid = true;
        Kept& k = sKept[sCache.slotOf(kept)];
        k.mOps = sOps;
        k.mCommands = sCommands;
        k.mRecords = sRecords;
        k.mTextures = sTextures;
        k.mParams = sCommandParams;
        sCurIndirect = &kept->mCommandBuffer;
        sCurRecords = &kept->mRecordBuffer;
        sCurTextures = &kept->mDataBuffer;
        sCache.countKept();
    }

    // Turns the pass's batches into ops (in the stock order) and commands. cache: FSPerfGPUDrivenBuildCache may
    // keep the pass, or use a kept one (key: which pass this is).
    void build(const PassInfo& pass, LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end, bool cache, U64 key)
    {
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD);
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD_LEGACY);
        FSGDDBuildScope scope(FS_GDD_BUILDER_LEGACY);
        scope.addBatches((size_t)(end - begin));
        sCurIndirect = &sIndirectBuffer;
        sCurRecords = &sRecordBuffer;
        sCurTextures = &sTextureBuffer;
        sUploads = UPLOAD_ALL;
        const U32 frame = LLFrameTimer::getFrameCount();
        FSGDDArena& arena = *sPassArena;
        arena.beginFrame(frame);
        const glm::mat4 proj = FSReverseZ::rasterProjection(gGL.getProjectionMatrix()); // as LLRender::syncMatrices gives the shaders (FSRenderReverseZ)
        const glm::mat4 view = get_current_modelview();

        U64 hash = 0;
        cache = cache && !sRiggedPass;
        if (cache)
        {
            hash = FSGDDBuildCache::hash((const uintptr_t*)begin, (size_t)(end - begin));
            FSGDDKeptPass* kept = sCache.find(key, (const uintptr_t*)begin, (size_t)(end - begin), hash);
            const bool used = kept && use_kept(*kept, arena, frame, proj, view);
            fsGDDLap(FS_GDD_STAGE_CACHE);
            if (used)
            {
                scope.setReused();
                sCache.countHit();
                return;
            }
            sTextureUses.clear();
            sArenaUses.clear();
            sMatrixUses.clear();
            sKeepable = true;
        }

        sOps.clear();
        sCommands.clear();
        sRecords.clear();
        sTextures.clear();
        sCommandParams.clear();
        sStockRigged.clear();
        fsGDDBeginTexturePass();
        sMatrices.begin(proj, view, true, true);   // texture matrix 0: identity
        U32 batches = 0;
        if (cache)
        {
            fsGDDRecordTextureUses(&sTextureUses);
            sMatrices.record(&sMatrixUses);
        }

        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            arena.prefetchAhead(i, end - i);
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);
            if (!pparams)
            {   // the stock loops skip these too
                continue;
            }
            LLDrawInfo& params = *pparams;
            ++sStatBatches;
            ++batches;

            // rigged: as pushRiggedBatches / pushRiggedMaskBatches, which draw nothing for a batch without avatar,
            // skin or palette yet (the masked one sets its minimum alpha first, which the next batch sets again)
            if (sRiggedPass && !FSSkinPaletteBuffer::drawable(params))
            {
                continue;
            }

            S32 reason = -1;
            const U32 tex_first = (U32)sTextures.size();
            U32 tex_matrix = 0;
            const FSGDDArenaEntry* entry = nullptr;
            if (!sRiggedPass && (params.mAvatar.notNull() || params.mSkinInfo.notNull()))
            {
                reason = R_RIGGED;
            }
            else if (!params.mCount || params.mVertexBuffer.isNull())
            {   // pushBatch draws nothing for these; it may still bind, so it gets them
                reason = R_EMPTY;
            }
            else if (LLRenderPass::fsFadeInHidden(params) != 0.f)
            {   // <FS:Trish/> fade-in, or out (FSRenderFadeLOD): its fs_fade_hide uniform is set per batch, the stock way
                reason = R_FADING;
            }
            else
            {
                fsGDDLap(FS_GDD_STAGE_WALK);
                // the header: the minimum alpha pushMaskBatches sets for this batch, and the texture count
                const F32 cutoff = pass.mMask ? params.mAlphaMaskCutoff : 0.f;
                U32 cutoff_bits = 0;
                memcpy(&cutoff_bits, &cutoff, sizeof(cutoff_bits));
                sTextures.push_back({ cutoff_bits, 0 });

                // the textures pushBatch(params, true, true) binds, in unit order
                if (params.mTextureList.size() > 1)
                {   // every texture of the list on its unit, the texture matrix untouched (identity)
                    for (const LLPointer<LLViewerTexture>& tex : params.mTextureList)
                    {
                        if (tex.isNull())
                        {   // its unit keeps an earlier batch's texture on the stock path
                            reason = R_NO_TEXTURE;
                            break;
                        }
                        const U64 handle = handle_of(tex.get(), frame);
                        if (!handle)
                        {
                            reason = R_HANDLE;
                            break;
                        }
                        push_handle(handle);
                    }
                }
                else if (params.mTexture.isNull())
                {   // unit 0 unbound on the stock path
                    reason = R_NO_TEXTURE;
                }
                else
                {   // one texture on unit 0, and the texture matrix if there is one
                    const U64 handle = handle_of(params.mTexture.get(), frame);
                    if (!handle)
                    {
                        reason = R_HANDLE;
                    }
                    else
                    {
                        push_handle(handle);
                        fsGDDLap(FS_GDD_STAGE_TEXTURES);
                        tex_matrix = sMatrices.textureIndex(params.mTextureMatrix.get());
                        fsGDDLap(FS_GDD_STAGE_MATRICES);
                    }
                }
                fsGDDLap(FS_GDD_STAGE_TEXTURES);

                if (reason < 0)
                {
                    sTextures[tex_first].mY = (U32)sTextures.size() - tex_first - 1;
                    U32 fail = 0;
                    if (pass.mNormals && !(params.mVertexBuffer->getTypeMask() & LLVertexBuffer::MAP_NORMAL))
                    {   // checked before the arena, which would copy the buffer only for it to be turned down here
                        reason = R_NORMALS;
                    }
                    else if (!(entry = arena.find(params.mVertexBuffer.get(), frame, &fail)))
                    {
                        reason = R_ARENA + fail;
                    }
                    else if (pass.mNormals && !(entry->mTypeMask & LLVertexBuffer::MAP_NORMAL))
                    {
                        reason = R_NORMALS;
                    }
                    else if (params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
                    {   // never for a valid batch; anything else is left to the stock path rather than read past the copy
                        reason = R_RANGE;
                    }
                }
                fsGDDLap(FS_GDD_STAGE_ARENA);
            }

            if (reason >= 0)
            {
                sTextures.resize(tex_first);
                ++sStatReasons[reason];
                sOps.push_back({ true, 0, 0, &params });
                if (sRiggedPass)
                {
                    sStockRigged.push_back(&params);
                }
                if (reason == R_RIGGED || reason == R_FADING || reason >= R_ARENA)
                {   // a rigged batch's avatar mustn't be held; the others may come out differently next frame
                    sKeepable = false;
                }
                continue;
            }

            const U32 model_matrix = sMatrices.modelIndex(params.mModelMatrix, proj, view);
            fsGDDLap(FS_GDD_STAGE_MATRICES);
            ++sStatDrawn;
            const U32 command = (U32)sCommands.size();
            sCommands.push_back({ params.mCount, 1, entry->mIndex + params.mOffset, (GLint)entry->mVertex, command });
            // skin SSS: the skin exclusion rides in the palette offset's top bit (assign_palettes keeps it)
            sRecords.push_back({ model_matrix, tex_matrix, tex_first,
                                 sRiggedPass && LLRenderPass::fsSkinExcluded(params) ? FS_GDD_SKIN_EXCLUDE_BIT : 0 });
            sCommandParams.push_back(&params);
            if (cache)
            {
                sArenaUses.push_back({ params.mVertexBuffer.get(), entry->mVertex, entry->mIndex });
            }

            Op* last = sOps.empty() ? nullptr : &sOps.back();
            if (last && !last->mStock)
            {
                ++last->mCount;
            }
            else
            {
                sOps.push_back({ false, command, 1, nullptr });
            }
            fsGDDLap(FS_GDD_STAGE_RECORDS);
        }

        if (cache)
        {
            fsGDDRecordTextureUses(nullptr);
            sMatrices.record(nullptr);
            if (sKeepable && !sCommands.empty())
            {
                keep(key, hash, begin, end, batches);
            }
            else
            {
                sCache.countUnkept();
            }
            fsGDDLap(FS_GDD_STAGE_CACHE);
        }
    }

    // the arena, the record attribute and the storage buffers for the slot's program (bound)
    void setup_inputs(U32 slot)
    {
        // the attributes go into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        if (sRiggedPass)
        {   // the skinned branches of diffuseV.glsl and fullbrightV.glsl project with the projection_matrix uniform
            // (the static ones only read the records' matrices). Only gGL's sync sets it, which every stock draw
            // goes through and a multi-draw doesn't: left unset it is all zeros and every rigged vertex collapses
            // to one point.
            gGL.syncMatrices();
        }
        if (sReplayPass >= 0)
        {   // FSRenderGPUOcclusionCulling phase 2: the copies of this pass's inputs
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_MATRIX_BINDING, FSGPUOcclusion::replayMatrices(sReplayPass));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_TEXTURE_BINDING, FSGPUOcclusion::replayMaterials(sReplayPass));
        }
        else
        {
            sMatrices.bindBase(FS_GDD_LEGACY_MATRIX_BINDING);
            sCurTextures->bindBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_TEXTURE_BINDING);
        }
        if (sRiggedPass)
        {   // the palette buffer whole; the records hold the offsets
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_SKIN_BINDING, FSSkinPaletteBuffer::glName());
        }
        if (sIndirectOverride)
        {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectOverride);
        }
        else
        {
            sCurIndirect->bind(GL_DRAW_INDIRECT_BUFFER);
        }
        const GLuint record = (GLuint)sRecordAttrib[slot];
        if (sReplayPass >= 0)
        {
            glBindBuffer(GL_ARRAY_BUFFER, FSGPUOcclusion::replayRecords(sReplayPass));
        }
        else
        {
            sCurRecords->bind(GL_ARRAY_BUFFER);
        }
        glEnableVertexAttribArray(record);
        glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), sReplayPass >= 0 ? (const void*)0 : sCurRecords->at(0));
        glVertexAttribDivisor(record, 1);
        // every attribute the program reads (bind_inputs checked the arena holds them), texture_index included
        sPassArena->bindForDraw(program_of(slot).mAttributeMask);
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 slot)
    {
        const GLuint record = (GLuint)sRecordAttrib[slot];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_MATRIX_BINDING, 0);
        if (sRiggedPass)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_SKIN_BINDING, 0);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_LEGACY_TEXTURE_BINDING, 0);  // also unbinds the generic binding
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    }

    // Where the multi-draws find the command `offset` bytes in
    const void* indirect_at(size_t offset)
    {
        return sIndirectOverride ? (const void*)offset : sCurIndirect->at(offset);
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
            if (!op.mStock && op.mCount)
            {
                sRanges.push_back({ op.mFirst, op.mCount });
            }
        }
        FSGPUOcclusion::PassInputs inputs;
        inputs.mRanges = sRanges.data();
        inputs.mRangeCount = (U32)sRanges.size();
        inputs.mKind = sRiggedPass ? FSGPUOcclusion::KIND_RIGGED : FSGPUOcclusion::KIND_LEGACY;
        inputs.mCommands = sCurIndirect->name();
        inputs.mCommandsOffset = sCurIndirect->offset();
        inputs.mCommandCount = (U32)sCommands.size();
        inputs.mRecords = sCurRecords->name();
        inputs.mRecordsOffset = sCurRecords->offset();
        inputs.mRecordBytes = sRecords.size() * sizeof(Record);
        inputs.mRecordStride = sizeof(Record) / sizeof(U32);
        inputs.mMatrixField = 0;    // Record::mMatrix: the modelview, the modelview projection after it
        inputs.mMVPOffset = 1;
        inputs.mMatrices = sMatrices.buffer().name();
        inputs.mMatricesOffset = sMatrices.buffer().offset();
        inputs.mMatrixBytes = sMatrices.size() * sizeof(glm::mat4);
        inputs.mMaterials = sCurTextures->name();     // the texture lists
        inputs.mMaterialsOffset = sCurTextures->offset();
        inputs.mMaterialBytes = sTextures.size() * sizeof(FSGDDTexEntry);
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

    // the stock way, as pushBatches / pushMaskBatches draw a batch (pushRiggedBatches / pushRiggedMaskBatches
    // for a rigged pass: its palette's range first; build left only batches that have one)
    void push_stock(LLRenderPass& pool, const PassInfo& pass, LLDrawInfo& params)
    {
        if (pass.mMask)
        {
            LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(params.mAlphaMaskCutoff);
        }
        if (sRiggedPass && !LLRenderPass::uploadMatrixPalette(params.mAvatar, params.mSkinInfo.get()))
        {
            return;
        }
        pool.pushBatch(params, true, true);
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

    // Draws the ops. The stock program is bound on entry and on return. False if the GPU-driven program
    // couldn't be bound before anything was drawn (then nothing was). cull: FSRenderGPUOcclusionCulling phase 1
    // for this pass (occ: its occlusion index, or -1 when it wasn't culled).
    bool execute(LLRenderPass& pool, U32 slot, U32& calls, U32& switches, bool cull, S32& occ)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy execute");
        LL_PROFILE_GPU_ZONE("gdd legacy");
        // the rigged pass apart: zone "gbuffer rigged" of the FSGPUTimings log, inside the pass's zone
        std::optional<LLFSGPUZone::Scope> rigged_zone;
        if (sRiggedPass)
        {
            rigged_zone.emplace("gbuffer rigged");
        }
        const PassInfo& pass = pass_of(slot);

        // a kept pass used again (FSPerfGPUDrivenBuildCache) has its commands, records and texture lists up already
        if (sUploads & UPLOAD_COMMANDS)
        {
            sCurIndirect->upload(GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(FSGDDDrawCommand));
        }
        if (sUploads & UPLOAD_RECORDS)
        {
            sCurRecords->upload(GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        }
        sMatrices.upload();
        if (sUploads & UPLOAD_TEXTURES)
        {
            sCurTextures->upload(GL_SHADER_STORAGE_BUFFER, sTextures.data(), sTextures.size() * sizeof(FSGDDTexEntry));
        }
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

        LLGLSLShader* stock = stock_of(slot);
        bool gdd = false;   // the GPU-driven program is bound
        bool drew = false;
        U32 next_range = 0;     // the pass's multi-draw ops so far (multi_draw)

        for (const Op& op : sOps)
        {
            const U32 range = !op.mStock && op.mCount ? next_range++ : 0;
            if (op.mStock)
            {
                if (gdd)
                {
                    finish_inputs(slot);
                    stock->bind();
                    gdd = false;
                    ++switches;
                }
                push_stock(pool, pass, *op.mParams);
                drew = true;
                continue;
            }

            if (sRiggedPass && FSSkinPaletteBuffer::generation() != sPaletteGeneration)
            {   // the palette buffer was replaced after the offsets were taken (not expected: place() put every
                // palette of the pass in it); the run the stock way, which binds each palette again
                LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenRigged: the palette buffer was replaced mid legacy pass" << LL_ENDL;
                if (op.mCount)
                {
                    mark_stock(range);
                }
                if (gdd)
                {
                    finish_inputs(slot);
                    stock->bind();
                    gdd = false;
                    ++switches;
                }
                for (U32 c = op.mFirst; c < op.mFirst + op.mCount; ++c)
                {
                    push_stock(pool, pass, *sCommandParams[c]);
                }
                drew = true;
                continue;
            }

            if (!gdd)
            {
                if (!bind_program(slot))
                {
                    stock->bind();
                    if (!drew)
                    {
                        return false;
                    }
                    // not expected (fsGDDDrawLegacy bound the program once already); the rest of the pass goes
                    // the stock way, in order
                    LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenLegacy: a program failed to bind mid pass" << LL_ENDL;
                    U32 rest_range = op.mCount ? range : next_range;   // this op's range, or the next one
                    for (size_t i = &op - sOps.data(); i < sOps.size(); ++i)
                    {
                        const Op& rest = sOps[i];
                        if (rest.mStock)
                        {
                            push_stock(pool, pass, *rest.mParams);
                        }
                        else
                        {
                            if (rest.mCount)
                            {
                                mark_stock(rest_range++);
                            }
                            for (U32 c = rest.mFirst; c < rest.mFirst + rest.mCount; ++c)
                            {
                                push_stock(pool, pass, *sCommandParams[c]);
                            }
                        }
                    }
                    return true;
                }
                ++switches;
                gdd = true;
                setup_inputs(slot);
            }
            multi_draw(op, range);
            ++calls;
            drew = true;
        }
        if (gdd)
        {
            finish_inputs(slot);
            stock->bind();
        }
        STOP_GLERROR;
        return true;
    }

    // FSRenderGPUOcclusionCulling phase 2 of a pass (FSGPUOcclusion::flushPhaseTwo): its runs again, reading the
    // phase 2 commands and the copies of its inputs. Batches drawn the stock way were drawn in phase 1.
    void replay_phase_two(S32 occ, U32 user)
    {
        if (user >= sReplayUsed || sReplayFrame != LLFrameTimer::getFrameCount())
        {
            return;
        }
        Replay& replay = sReplays[user];
        const U32 slot = replay.mSlot;
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy phase 2");
        LL_PROFILE_GPU_ZONE("gdd legacy phase 2");
        if (replay.mRigged && FSSkinPaletteBuffer::generation() != replay.mGeneration &&
            !FSGPUOcclusion::refreshPalettes(occ, replay.mParams, replay.mRecords, sizeof(Record) / sizeof(U32), 3,
                                             replay.mGeneration))
        {   // not expected: place() found no buffer, or it never held still
            LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: the joint palettes of a rigged legacy pass could not be "
                                       << "put in the palette buffer again for its phase 2" << LL_ENDL;
            return;
        }
        sRiggedPass = slot_rigged(slot);
        std::optional<LLFSGPUZone::Scope> rigged_zone;
        if (sRiggedPass)
        {
            rigged_zone.emplace("gbuffer rigged");
        }
        sPassArena = sRiggedPass ? &sRiggedArena : &sArena;
        sReplayPass = occ;
        sIndirectOverride = FSGPUOcclusion::replayCommands(occ);
        LLVertexBuffer::unbind();
        LLGLDepthTest depth(GL_TRUE, GL_TRUE);
        LLGLDisable blend(GL_BLEND);

        program_of(slot).mUniformsDirty = true;
        if (!bind_program(slot))
        {   // not expected: it drew in phase 1 this frame
            LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: a legacy program failed to bind in phase 2" << LL_ENDL;
        }
        else
        {
            setup_inputs(slot);
            U32 calls = 0;
            U32 range = 0;
            for (const Op& op : replay.mOps)
            {
                const U32 op_range = range++;
                if (!op.mCount)
                {   // drawn the stock way in phase 1 (keep_for_phase_two)
                    continue;
                }
                multi_draw(op, op_range);
                ++calls;
            }
            finish_inputs(slot);
            sStatCalls += calls;
        }
        sReplayPass = -1;
        sIndirectOverride = 0;
        STOP_GLERROR;
    }

    // Keeps the runs of a pass phase 1 culled, for its phase 2
    void keep_for_phase_two(S32 occ, U32 slot)
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
        replay.mSlot = slot;
        replay.mOps.clear();
        U32 range = 0;
        for (const Op& op : sOps)
        {
            if (!op.mStock && op.mCount)
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

    // FSPerfGPUDrivenLegacyVerify: the batches drawn from the arena again, the stock way, colour and depth
    // writes off, depth test GL_LESS against what the multi-draws left. The stock program must be bound.
    void verify_against_stock(LLRenderPass& pool, const PassInfo& pass)
    {
        if (sCommandParams.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy verify");

        static LLFrameTimer error_timer;
        fsGDDCheckGLError(error_timer, "FSPerfGPUDrivenLegacyVerify", pass.mName);

        // arena copies against their sources, a couple per pass
        sStatMismatches += fsGDDVerifyArenaCopies(*sPassArena, sCommandParams,
                                                  [](LLDrawInfo* params) { return params->mVertexBuffer.get(); },
                                                  "FSPerfGPUDrivenLegacyVerify");

        // the occlusion check needs the multi-draws' depth in the buffer
        GLboolean depth_test = GL_FALSE;
        GLboolean depth_write = GL_FALSE;
        glGetBooleanv(GL_DEPTH_TEST, &depth_test);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
        if (!depth_test || !depth_write)
        {
            ++sStatUnverified;
            return;
        }

        const GLuint samples = fsGDDNearerSamples(sQuery, false, [&]()
        {
            for (LLDrawInfo* params : sCommandParams)
            {
                push_stock(pool, pass, *params);
            }
        });
        if (samples)
        {
            ++sStatMismatches;
            static LLFrameTimer warn_timer;
            if (fsGDDWarnDue(warn_timer))
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenLegacyVerify: " << samples << " samples of " << sCommandParams.size()
                                      << " stock batches (" << pass.mName << ") are nearer than the multi-draw depth; "
                                      << sStatMismatches << " mismatches so far" << LL_ENDL;
            }
        }
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
        LL_INFOS("FSGPUDrivenLegacy") << "GPU-driven legacy, last 10 s: " << sStatPasses << " passes, " << sStatBatches
                                      << " batches (" << sStatDrawn << " from the arena) in " << sStatCalls
                                      << " multi-draw calls, " << sStatSwitches << " program switches; stock path for: "
                                      << reasons << "; arena "
                                      << fsGDDArenaSummary(sArena) << "; rigged arena "
                                      << fsGDDArenaSummary(sRiggedArena) << "; "
                                      << sStatMismatches << " verify mismatches, " << sStatUnverified
                                      << " passes without depth writes (occlusion check skipped)" << LL_ENDL;
        sStatPasses = sStatBatches = sStatDrawn = sStatCalls = sStatSwitches = sStatUnverified = 0;
        sArena.clearCounts();
        sRiggedArena.clearCounts();
    }
}

bool fsGDDDrawLegacy(LLRenderPass& pool, U32 type)
{
    // before any early return, so idle handles go non resident and away also while the PBR pass is off;
    // it does something every 120 frames, and is called once a frame rather than once per pass
    static U32 s_swept_frame = U32_MAX;
    const U32 sweep_frame = LLFrameTimer::getFrameCount();
    if (sweep_frame != s_swept_frame)
    {
        s_swept_frame = sweep_frame;
        fsGDDSweepHandles(sweep_frame);
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenLegacy", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !fsGDDBindlessReady() || LLPipeline::sShadowRender ||
        LLPipeline::sRenderingHUDs)
    {
        return false;
    }
    // the slot: the pass, or (FSPerfGPUDrivenRigged) its rigged pass
    U32 slot = SLOT_COUNT;
    for (U32 p = 0; p < P_COUNT; ++p)
    {
        if (sPasses[p].mType == type)
        {
            slot = p;
            break;
        }
        if (sPasses[p].mRiggedType == type)
        {
            slot = p + P_COUNT;
            break;
        }
    }
    if (slot == SLOT_COUNT || (slot_rigged(slot) && !fsGDDRiggedEnabled()))
    {
        return false;
    }
    const PassInfo& pass = pass_of(slot);
    LLGLSLShader* stock = stock_of(slot);
    // the HUD programs and anything else bound in place of the stock one keep the stock path
    if (!stock || LLGLSLShader::sCurBoundShaderPtr != stock || !program_usable(slot))
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
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenLegacyVerify", false);
    static LLCachedControl<bool> stats(gSavedSettings, "FSPerfGPUDrivenLegacyStats", false);

    // The first bind finds the program's inputs; do that now, so a failure leaves the whole pass to the
    // stock loop rather than part of it
    if (!sInputsReady[slot])
    {
        const bool ready = bind_program(slot);
        stock->bind();
        if (!ready)
        {
            return false;
        }
    }

    // The environment uniforms (LLEnvironment marks the programs of the shader list dirty when they change,
    // and the stock program took them when the pool bound it); ours isn't in that list, so it takes the
    // current values at its first bind in each pass
    program_of(slot).mUniformsDirty = true;

    sRiggedPass = slot_rigged(slot);
    sPassArena = sRiggedPass ? &sRiggedArena : &sArena;
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd legacy build");
        // FSPerfGPUDrivenBuildCache: not while verifying, which checks what a build makes
        const bool cache = !verify && FSGDDBuildCache::enabled();
        build(pass, begin, end, cache, ((U64)type << 32) | slot);
    }
    if (sRiggedPass && !sCommands.empty() && !assign_palettes())
    {   // no buffer, or it never held still: the stock loop draws the pass
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenRigged: the joint palettes could not be put in the palette buffer; "
                                   << "a rigged legacy pass is drawn as before" << LL_ENDL;
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
    // FSRenderGPUOcclusionCulling: the G-buffer passes of the main camera, rigged ones too (the fullbright ones
    // draw after the G-buffer, where it never culls); not while verifying, which compares against everything the
    // stock path draws
    const bool cull = pass.mNormals && !verify && FSGPUOcclusion::passActive();
    S32 occ = -1;
    if (!execute(pool, slot, calls, switches, cull, occ))
    {
        return false;
    }
    if (occ >= 0)
    {
        keep_for_phase_two(occ, slot);
    }

    if (verify)
    {
        verify_against_stock(pool, pass);
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
