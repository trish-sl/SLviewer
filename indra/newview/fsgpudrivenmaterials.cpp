/**
 * @file fsgpudrivenmaterials.cpp
 * @brief GPU-driven draws for the deferred legacy material passes (FSPerfGPUDrivenMaterials)
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

// The stock legacy material passes (LLDrawPoolMaterials::renderDeferred: PASS_MATERIAL, PASS_SPECMAP,
// PASS_NORMMAP, PASS_NORMSPEC and their alpha mask and emissive variants, the surfaces with a legacy normal or
// specular map, or environment shine on a material) cost, for every batch: up to four uniforms (specular
// colour, environment intensity, minimum alpha, fullbright), up to three texture binds (diffuse, normal and
// specular map), a model matrix (modelview, modelview projection and normal matrix uniforms), a texture matrix
// load and reset when the texture is animated, a vertex buffer bind and a glDrawRangeElements.
//
// With FSPerfGPUDrivenMaterials on, the static batches come out of a vertex arena instead (fsgddarena.h: a GPU
// side copy of the position, normal, texcoord 0, colour and, where the buffer has them, tangent, texcoord 1
// and texcoord 2 regions, and the 16 bit indices of their vertex buffers), and each batch is one 20 byte
// indirect draw command plus a record: which model matrices (in a storage buffer, the matrices the stock path
// would have computed, computed the same way), which texture matrix (identity, or the batch's animation), and
// which material (a second storage buffer: the specular colour, environment intensity, minimum alpha and
// fullbright value the stock loop would set, and the bindless handles of the textures it would bind).
// materialV.glsl built with FS_GDD_MATERIAL reads them and hands the material to materialF.glsl, which reads
// it in place of its uniforms and samplers; everything else in the shaders is the stock code.
//
// One GPU-driven program per stock permutation, so the passes stay as they are: the batches of each pass are
// drawn in their stock order, one glMultiDrawElementsIndirect per run of batches from the arena. A batch that
// can't come from it (rigged, empty, a texture missing or without a handle (not loaded, being made,
// options pending, media), 32 bit indices, buffer mapped or lacking an attribute the program reads, arena
// full or the copy budget used up) is drawn by the stock code in its place. Blended passes stay entirely stock.
//
// FSPerfGPUDrivenRigged (with FSPerfRiggedPaletteBuffer, fsskinpalettebuffer.h): the rigged passes (type + 1:
// avatar clothing, bodies, rigged attachments) the same way, with the rigged permutations
// (gDeferredMaterialProgram[index + 0x10]) built with FS_GDD_MATERIAL and LLShaderFeatures::mFSGDDSkinning,
// which links them with the copy of objectSkinV.glsl built with FS_GDD_RIGGED (LLViewerShaderMgr::
// loadBasicShaders). Their batches come from a second arena that also holds the joint weights, and each record
// names (its w, Record::mSkin) where the batch's palette starts in this frame's palette buffer, which the pass
// binds whole as a storage buffer: the palette the stock loop would bind (the same build of the same
// MatrixPaletteCache, the same floats) in the layout the stock programs read through FSSkinPalette, and the
// same skinning code reads it. Model matrices are the stock ones too (a rigged batch's is mostly null: the
// view matrix alone, the palette carries the rest). A batch without avatar, skin or palette yet is skipped, as
// the stock loop skips it. The palettes are all put in the buffer before the pass draws, the stock fallbacks'
// as well (their bind then only finds them); should the buffer still be replaced mid pass (it grows by starting
// over in new storage, FSSkinPaletteBuffer::generation), the multi-draws left are drawn the stock way.
//
// FSPerfGPUDrivenPBRGroupRuns gives that order up here too: the stock fallbacks spread through a pass cut it
// into many runs, and every switch between a run and a stock batch costs a program bind and the inputs set up
// again. A run has no state of its own (the material of every batch is in its record), so grouped, all batches
// from the arena go out as one multi-draw and the stock fallbacks after them, in their stock order. The depth
// test decides what a G-buffer pass leaves, so only where two surfaces have exactly the same depth can a
// different one win than in the stock order.
//
// The texture handles are those of FSPerfGPUDrivenPBRBindless (fsgpudrivenpbr.cpp), looked up once per
// texture per pass through the cache FSPerfGPUDrivenLegacy keeps (fsGDDPassTextureHandle); this needs bindless
// textures, which are only used on NVIDIA.
//
// FSPerfGPUDrivenMaterialsVerify: as FSPerfGPUDrivenLegacyVerify, after the pass the batches it drew from the
// arena are drawn again the stock way with colour and depth writes off and depth test GL_LESS under a
// GL_SAMPLES_PASSED query; it also reads back a couple of arena entries per pass, checks for GL errors and
// logs statistics every 10 seconds (FSPerfGPUDrivenMaterialsStats logs only the statistics).
//
// FSPerfGPUDrivenBuildCache (fsgddcommon.cpp): a non rigged pass given the same batches as in an earlier frame uses
// the commands, records and materials built then, from their own buffers, once the texture handles and arena
// copies they name are found to be the same; only its matrices are looked up again.
//
// FSRenderGPUOcclusionCulling (fsgpuocclusion.h), the opaque, alpha mask and emissive passes of the main camera's
// G-buffer: as in the PBR pass (fsgpudrivenpbr.cpp), the pass's commands go through the phase 1 cull first and
// the multi-draws read the culled copy (sIndirectOverride), which keeps only the batches visible last frame. The
// pass's runs are kept, and once every opaque pool has drawn, replay_phase_two draws them again with the phase 2
// commands (the batches found visible only then) from copies of the pass's records, matrices and materials
// (sReplayPass). Batches drawn the stock way are drawn in phase 1 only, as before. The rigged passes too
// (FSPerfGPUDrivenRigged; their boxes come from their avatars): should the palette buffer be replaced before
// phase 2, their palettes are put in it again and the records' offsets rewritten (FSGPUOcclusion::refreshPalettes).

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenmaterials.h"
#include "fsreversez.h"

#include "fsgddarena.h"
#include "fsgddcommon.h"
#include "fsgpudrivenlegacy.h"
#include "fsgpudrivenpbr.h"
#include "fsgpuocclusion.h"
#include "fsskinpalettebuffer.h"

#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llmaterial.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <cstring>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    // the other passes use the same bindings; each pass binds its buffers and clears them again
    constexpr GLuint FS_GDD_MAT_MATRIX_BINDING = 5;     // shader storage binding of FSGDDMaterialMatrices
    constexpr GLuint FS_GDD_MAT_MATERIAL_BINDING = 6;   // and of FSGDDMaterialRecords
    constexpr GLuint FS_GDD_MAT_SKIN_BINDING = 7;       // and of FSGDDSkinPalettes (objectSkinV.glsl, rigged)
    constexpr U32 FS_GDD_MAT_RIGGED = LLMaterial::SHADER_COUNT;    // index bit of the rigged permutations (0x10)
    constexpr U32 FS_GDD_MAT_PROGRAMS = LLMaterial::SHADER_COUNT * 2; // by gDeferredMaterialProgram index
    // the attributes the arenas can feed; a program wanting any other is not used (the weights only the rigged
    // arena has; the rigged programs read them, the others don't)
    constexpr U32 FS_GDD_MAT_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0 |
                                       LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 |
                                       LLVertexBuffer::MAP_TEXCOORD2 | LLVertexBuffer::MAP_WEIGHT4;

    // everything materialV.glsl reads; tangents and the second and third texture coordinates only where the
    // buffer has them (a program that reads them turns a buffer without them down). An eighth of
    // FSPerfGPUDrivenMaterialsArenaMB for indices.
    FSGDDArena sArena("FSPerfGPUDrivenMaterials arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                        LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_TANGENT, LLVertexBuffer::TYPE_TEXCOORD1,
                        LLVertexBuffer::TYPE_TEXCOORD2 },
                      "FSPerfGPUDrivenMaterialsArenaMB", 256, 8,
                      LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2);
    // FSPerfGPUDrivenRigged: the same plus the joint weights, for the rigged passes, so rigged buffers (many, and
    // large) neither crowd the static ones out nor make every vertex of the first arena carry weights. The same
    // size limit, its own.
    FSGDDArena sRiggedArena("FSPerfGPUDrivenRigged arena",
                            { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                              LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_WEIGHT4, LLVertexBuffer::TYPE_TANGENT,
                              LLVertexBuffer::TYPE_TEXCOORD1, LLVertexBuffer::TYPE_TEXCOORD2 },
                            "FSPerfGPUDrivenMaterialsArenaMB", 256, 8,
                            LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2);

    bool is_rigged(U32 index)
    {
        return (index & FS_GDD_MAT_RIGGED) != 0;
    }

    FSGDDArena& arena_for(U32 index)
    {
        return is_rigged(index) ? sRiggedArena : sArena;
    }

    struct Record       // fs_gdd_draw
    {
        U32 mMatrix;    // modelview; modelview projection and normal matrix follow
        U32 mTexMatrix;
        U32 mMaterial;
        U32 mSkin;      // rigged: the palette's first vec4 in the palette buffer (FSSkinPaletteBuffer::offsetOf / 16)
    };

    struct Material     // FSGDDLegacyMaterial, std430
    {
        F32 mSpecular[4];   // specular_color
        F32 mParams[4];     // env_intensity, minimum_alpha, emissive_brightness, 0
        U32 mTex[8];        // handles (low, high): diffuseMap, bumpMap, specularMap, unused
    };
    static_assert(sizeof(Material) == 64, "FSGDDLegacyMaterial layout");

    struct Op
    {
        bool mStock;            // drawn the stock way (push_stock(*mParams))
        U32 mFirst;             // first command
        U32 mCount;             // commands
        LLDrawInfo* mParams;
    };

    // why a batch went the stock way
    enum EReason
    {
        R_RIGGED = 0,
        R_EMPTY,
        R_NO_TEXTURE,
        R_HANDLE,
        R_ATTRIBUTES,
        R_RANGE,
        R_FADING,                       // fading in (FSRenderFadeIn)
        R_ARENA,                        // + FSGDDArena::EFail
        R_COUNT = R_ARENA + FSGDDArena::FAIL_COUNT
    };
    const char* reason_name(U32 reason)
    {
        static const char* names[] = { "rigged", "empty", "no texture", "texture without handle",
                                       "attributes the program reads", "index range", "fading in" };
        return reason < R_ARENA ? names[reason] : FSGDDArena::failName(reason - R_ARENA);
    }

    LLGLSLShader sPrograms[FS_GDD_MAT_PROGRAMS];    // by gDeferredMaterialProgram index
    GLint sRecordAttrib[FS_GDD_MAT_PROGRAMS];       // location of fs_gdd_draw
    S32 sInputsReady[FS_GDD_MAT_PROGRAMS] = {};     // bind_inputs done: 0 not yet, 1 yes, -1 failed

    FSGDDBuffer sIndirectBuffer;
    FSGDDBuffer sRecordBuffer;
    FSGDDBuffer sMaterialBuffer;
    GLuint sQuery = 0;

    // The buffers the pass being drawn reads (the ones above, or with FSPerfGPUDrivenBuildCache a kept pass's), and
    // which of them execute uploads to
    enum EUpload
    {
        UPLOAD_COMMANDS = 1,
        UPLOAD_RECORDS = 2,
        UPLOAD_MATERIALS = 4,
        UPLOAD_ALL = 7
    };
    FSGDDBuffer* sCurIndirect = &sIndirectBuffer;
    FSGDDBuffer* sCurRecords = &sRecordBuffer;
    FSGDDBuffer* sCurMaterials = &sMaterialBuffer;
    U32 sUploads = UPLOAD_ALL;

    std::vector<Op> sOps;
    std::vector<FSGDDDrawCommand> sCommands;
    std::vector<Record> sRecords;
    FSGDDMatrixTable sMatrices;
    std::vector<Material> sMaterials;
    std::vector<LLDrawInfo*> sCommandParams;    // per command: its batch (verify, and the stock fallback)
    // rigged pass: per command its palette, and the palettes of the batches drawn the stock way
    std::vector<const LLVOAvatar::MatrixPaletteCache*> sCommandPalettes;
    std::vector<const LLVOAvatar::MatrixPaletteCache*> sStockPalettes;
    U32 sPaletteGeneration = 0;                 // FSSkinPaletteBuffer::generation() the records' offsets hold for
    FSGDDArena* sVerifyArena = &sArena;         // the arena of the pass verify_against_stock checks
    bool sRiggedPass = false;                   // the pass being drawn is a rigged one

    // FSRenderGPUOcclusionCulling: the indirect buffer the multi-draws read instead of sIndirectBuffer (at offset
    // 0; 0 for none), the occlusion pass whose input copies setup_inputs binds (phase 2; -1 for none), the cull
    // entries, and per frame the program and runs of each culled pass for its phase 2
    GLuint sIndirectOverride = 0;
    S32 sReplayPass = -1;
    std::vector<FSGPUOcclusion::CullEntry> sCullEntries;
    S32 sCullPass = -1;     // phase 1: the occlusion index of the pass being drawn (-1 for none)
    std::vector<FSGPUOcclusion::Range> sRanges;    // its multi-draw ops' commands, in order
    struct Replay
    {
        U32 mIndex = 0;         // gDeferredMaterialProgram index
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
    // consecutive batches mostly share their matrices: the last answers skip the map lookups

    // The stock loop's state for the batches drawn the stock way: its uniform locations and texture units, and
    // what it last set (uniforms belong to the stock program, which the GPU-driven draws leave alone, and those
    // bind no textures, so the values stay current across them)
    struct StockState
    {
        bool mReady = false;
        GLint mIntensity = -1;
        GLint mBrightness = -1;
        GLint mMinAlpha = -1;
        GLint mSpecular = -1;
        S32 mDiffuseChannel = -1;
        S32 mSpecChannel = -1;
        S32 mNormChannel = -1;
        F32 mLastIntensity = 0.f;
        F32 mLastFullbright = 0.f;
        F32 mLastMinimumAlpha = 0.f;
        LLVector4 mLastSpecular;
        LLTexture* mLastNormalMap = nullptr;
        LLTexture* mLastSpecMap = nullptr;
        LLTexture* mLastDiffuse = nullptr;
    };
    StockState sStock;

    // FSPerfGPUDrivenBuildCache: the kept passes, and by their slot what the build made besides the buffers
    constexpr U32 FS_GDD_MAT_KEPT = 32;
    FSGDDBuildCache sCache(FS_GDD_BUILDER_MATERIALS, FS_GDD_MAT_KEPT);
    struct Kept
    {
        std::vector<Op> mOps;
        std::vector<FSGDDDrawCommand> mCommands;
        std::vector<Record> mRecords;
        std::vector<Material> mMaterials;
        std::vector<LLDrawInfo*> mParams;
    };
    std::vector<Kept> sKept(FS_GDD_MAT_KEPT);
    // what a build that may be kept looked up, and whether it may be
    std::vector<FSGDDTextureUse> sTextureUses;
    std::vector<FSGDDArenaUse> sArenaUses;
    std::vector<FSGDDMatrixUse> sMatrixUses;
    bool sKeepable = false;
    std::vector<U32> sRemap;

    // stats, every 10 seconds with FSPerfGPUDrivenMaterialsVerify or FSPerfGPUDrivenMaterialsStats
    U64 sStatBatches = 0;
    U64 sStatDrawn = 0;
    U64 sStatCalls = 0;
    U64 sStatSwitches = 0;
    U64 sStatPasses = 0;
    U64 sStatMaterials = 0;
    U64 sStatReasons[R_COUNT] = {};
    U64 sStatMismatches = 0;
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    // the G-buffer permutations: not blended (DIFFUSE_ALPHA_MODE_BLEND goes to the alpha pool); rigged ones only
    // when rigged says so
    bool wanted_program(U32 index, bool rigged)
    {
        return index < FS_GDD_MAT_PROGRAMS && (index & 0x3) != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND &&
               (rigged || !is_rigged(index));
    }

    // FSPerfGPUDrivenRigged may draw now: on, and the shaders now loaded read the palette buffer (the rigged
    // programs exist only if both held when they were built)
    bool rigged_enabled()
    {
        static LLCachedControl<bool> rigged(gSavedSettings, "FSPerfGPUDrivenRigged", false);
        return rigged && FSSkinPaletteBuffer::active();
    }

    bool create_program(U32 index)
    {
        const LLGLSLShader& stock = gDeferredMaterialProgram[index];
        LLGLSLShader& shader = sPrograms[index];
        shader.mName = llformat("FS GPU-Driven Material Shader %d", index);
        shader.mFeatures = stock.mFeatures;
        shader.mFeatures.mIndexedTextureChannels = 0;
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_NORMAL_MAP, DIFFUSE_ALPHA_MODE, ...), then ours
        shader.addPermutation("FS_GDD_MATERIAL", "1");
        // rigged (HAS_SKIN among the stock defines): the skinning object that reads the palettes from the storage buffer
        shader.mFeatures.mFSGDDSkinning = is_rigged(index);
        return shader.createShader();
    }

    // the storage block bindings, where the driver put the per command record, and whether the arena can feed
    // every attribute the program reads
    bool bind_inputs(LLGLSLShader& shader, bool rigged, GLint& record_attrib)
    {
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDMaterialMatrices");
        const GLuint materials = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDMaterialRecords");
        const GLuint palettes = rigged ? glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDSkinPalettes")
                                       : GL_INVALID_INDEX;
        record_attrib = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        if (matrices == GL_INVALID_INDEX || materials == GL_INVALID_INDEX || (rigged && palettes == GL_INVALID_INDEX) ||
            record_attrib < 0 || (shader.mAttributeMask & ~FS_GDD_MAT_ATTRIBS) || !(shader.mAttributeMask & LLVertexBuffer::MAP_VERTEX) ||
            (rigged != ((shader.mAttributeMask & LLVertexBuffer::MAP_WEIGHT4) != 0)))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_MAT_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, materials, FS_GDD_MAT_MATERIAL_BINDING);
        if (rigged)
        {
            glShaderStorageBlockBinding(shader.mProgramObject, palettes, FS_GDD_MAT_SKIN_BINDING);
        }
        return true;
    }

    // Binds a GPU-driven program; the first time, also finds its inputs. False (for good) when they aren't there.
    bool bind_program(U32 index)
    {
        LLGLSLShader& shader = sPrograms[index];
        shader.bind();
        if (LLGLSLShader::sCurBoundShaderPtr != &shader)
        {
            return false;
        }
        if (!sInputsReady[index])
        {
            sInputsReady[index] = bind_inputs(shader, is_rigged(index), sRecordAttrib[index]) ? 1 : -1;
            if (sInputsReady[index] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenMaterials: " << shader.mName << " lacks its storage blocks or "
                                      << "draw record attribute, or reads an attribute the arena doesn't hold; its "
                                      << "batches are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[index] > 0;
    }

    bool program_usable(U32 index)
    {
        return sPrograms[index].isComplete() && sInputsReady[index] >= 0;
    }
}

void fsGDDMaterialsUnloadShaders()
{
    for (U32 index = 0; index < FS_GDD_MAT_PROGRAMS; ++index)
    {
        sPrograms[index].unload();
        sInputsReady[index] = 0;
        sRecordAttrib[index] = -1;
    }
    fsGDDMaterialsReleaseGL();
}

void fsGDDMaterialsLoadShaders()
{
    fsGDDMaterialsUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenMaterials", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenMaterials: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; material batches are drawn as before" << LL_ENDL;
        return;
    }
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenMaterials: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; material batches are drawn as before" << LL_ENDL;
        return;
    }
    // without bindless textures every batch would still bind its textures, which is much of what this saves
    if (!fsGDDBindlessSetup())
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenMaterials: needs bindless textures (NVIDIA GPUs only); "
                                       << "material batches are drawn as before" << LL_ENDL;
        return;
    }

    // FSPerfGPUDrivenRigged: the rigged permutations too, when the shaders read the palette buffer, the skinning
    // object for them was built and a third storage block fits
    bool rigged = fsGDDRiggedWanted() && FSSkinPaletteBuffer::active();
    if (rigged && !LLShaderMgr::instance()->mVertexShaderObjects.count(LLShaderMgr::FS_GDD_SKIN_OBJECT))
    {
        rigged = false; // loadBasicShaders logged it
    }
    if (rigged && vs_blocks < 3)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenRigged: needs three shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; rigged material batches are drawn as before" << LL_ENDL;
        rigged = false;
    }

    for (U32 index = 0; index < FS_GDD_MAT_PROGRAMS; ++index)
    {
        if (!wanted_program(index, rigged) || gDeferredMaterialProgram[index].mShaderFiles.empty())
        {
            continue;
        }
        if (!create_program(index))
        {
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenMaterials: " << sPrograms[index].mName << " failed to build, "
                                      << "its batches are drawn as before" << LL_ENDL;
            sPrograms[index].unload();
        }
    }
}

bool fsGDDRiggedWanted()
{
    static LLCachedControl<bool> rigged(gSavedSettings, "FSPerfGPUDrivenRigged", false);
    static LLCachedControl<bool> materials(gSavedSettings, "FSPerfGPUDrivenMaterials", false);
    static LLCachedControl<bool> pbr(gSavedSettings, "FSPerfGPUDrivenPBR", false);
    static LLCachedControl<bool> legacy(gSavedSettings, "FSPerfGPUDrivenLegacy", false);
    static LLCachedControl<bool> alpha(gSavedSettings, "FSPerfGPUDrivenAlpha", false);
    return rigged && (materials || pbr || legacy || alpha) && FSSkinPaletteBuffer::wanted() && gGLManager.mFSHasGPUDrivenDraws;
}

bool fsGDDRiggedEnabled()
{
    static LLCachedControl<bool> rigged(gSavedSettings, "FSPerfGPUDrivenRigged", false);
    return rigged && FSSkinPaletteBuffer::active() && !LLPipeline::sImpostorRender;
}

bool fsGDDRiggedLoadable(S32 vs_blocks)
{
    return vs_blocks >= 3 && fsGDDRiggedWanted() && FSSkinPaletteBuffer::active() &&
           LLShaderMgr::instance()->mVertexShaderObjects.count(LLShaderMgr::FS_GDD_SKIN_OBJECT) != 0;
}

void fsGDDMaterialsReleaseGL()
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
    sMaterialBuffer.release();
    sMatrices.release();
    fsGDDDeleteQuery(sQuery);
}

//-----------------------------------------------------------------------------
// The pass

namespace
{
    void set_handle(Material& mat, U32 slot, U64 handle)
    {
        mat.mTex[slot * 2] = (U32)(handle & 0xFFFFFFFFull);
        mat.mTex[slot * 2 + 1] = (U32)(handle >> 32);
    }

    // The handle of a texture the stock loop binds, or the reason it can't be had
    S32 texture_of(LLViewerTexture* tex, U32 frame, U64& handle)
    {
        if (!tex)
        {   // the stock loop leaves the unit unbound, or as an earlier batch left it
            return R_NO_TEXTURE;
        }
        handle = fsGDDPassTextureHandle(tex, frame);
        return handle ? -1 : R_HANDLE;
    }

    // FSPerfGPUDrivenBuildCache: the kept pass, if what it names is still where it was (the texture handles, the
    // arena copies); then its ops, commands, records and materials are the pass's, its buffers the ones drawn from,
    // and the matrix table holds its matrices. False, with nothing taken, otherwise.
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
        sMaterials = k.mMaterials;
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
        sCurMaterials = &kept.mDataBuffer;
        sStatBatches += kept.mBatches;
        sStatDrawn += sCommands.size();
        sStatMaterials += sMaterials.size();
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
        k.mMaterials = sMaterials;
        k.mParams = sCommandParams;
        sCurIndirect = &kept->mCommandBuffer;
        sCurRecords = &kept->mRecordBuffer;
        sCurMaterials = &kept->mDataBuffer;
        sCache.countKept();
    }

    // Turns the pass's batches into ops (in the stock order, or grouped: one op for all the commands, then the
    // stock fallbacks) and commands. cache: FSPerfGPUDrivenBuildCache may keep the pass, or use a kept one (key:
    // which pass this is).
    void build(U32 index, LLCullResult::drawinfo_iterator begin, LLCullResult::drawinfo_iterator end, bool grouped, bool cache,
               U64 key)
    {
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD);
        LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD_MATERIALS);
        FSGDDBuildScope scope(FS_GDD_BUILDER_MATERIALS);
        scope.addBatches((size_t)(end - begin));
        sCurIndirect = &sIndirectBuffer;
        sCurRecords = &sRecordBuffer;
        sCurMaterials = &sMaterialBuffer;
        sUploads = UPLOAD_ALL;
        const U32 frame = LLFrameTimer::getFrameCount();
        const bool rigged = is_rigged(index);
        FSGDDArena& arena = arena_for(index);
        arena.beginFrame(frame);
        const glm::mat4 proj = FSReverseZ::rasterProjection(gGL.getProjectionMatrix()); // as LLRender::syncMatrices gives the shaders (FSRenderReverseZ)
        const glm::mat4 view = get_current_modelview();

        U64 hash = 0;
        cache = cache && !rigged;
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
        sMaterials.clear();
        sCommandParams.clear();
        sCommandPalettes.clear();
        sStockPalettes.clear();
        fsGDDBeginTexturePass();
        sMatrices.begin(proj, view, true, true);   // texture matrix 0: identity
        U32 batches = 0;
        if (cache)
        {
            fsGDDRecordTextureUses(&sTextureUses);
            sMatrices.record(&sMatrixUses);
        }

        // the maps the stock loop binds: those the stock program samples (its texture channels)
        const LLGLSLShader& stock = gDeferredMaterialProgram[index];
        const bool normal_map = stock.getTextureChannel(LLShaderMgr::BUMP_MAP) > -1;
        const bool spec_map = stock.getTextureChannel(LLShaderMgr::SPECULAR_MAP) > -1;
        // the attributes the program reads that a buffer may lack
        const U32 needed = sPrograms[index].mAttributeMask & FS_GDD_MAT_ATTRIBS;

        for (LLCullResult::drawinfo_iterator i = begin; i != end; )
        {
            arena.prefetchAhead(i, end - i);
            LLDrawInfo* pparams = *i;
            LLCullResult::increment_iterator(i, end);
            if (!pparams)
            {
                continue;
            }
            LLDrawInfo& params = *pparams;
            ++sStatBatches;
            ++batches;

            S32 reason = -1;
            const FSGDDArenaEntry* entry = nullptr;
            Material mat = {};
            const LLVOAvatar::MatrixPaletteCache* palette = nullptr;
            if (rigged)
            {
                // as LLRenderPass::uploadMatrixPalette: without avatar, skin or palette (skin not loaded yet) the
                // stock loop doesn't draw the batch
                if (params.mAvatar.isNull() || params.mSkinInfo.isNull())
                {
                    continue;
                }
                palette = &params.mAvatar->updateSkinInfoMatrixPalette(params.mSkinInfo.get());
                if (palette->mMatrixPalette.empty())
                {
                    continue;
                }
            }
            if (!rigged && (params.mAvatar.notNull() || params.mSkinInfo.notNull()))
            {
                reason = R_RIGGED;
            }
            else if (!params.mCount || params.mVertexBuffer.isNull())
            {   // the stock loop would still bind for these; it gets them
                reason = R_EMPTY;
            }
            else if (LLRenderPass::fsFadeInHidden(params) != 0.f)
            {   // <FS:Trish/> fade-in, or out (FSRenderFadeLOD): its fs_fade_hide uniform is set per batch, the stock way
                reason = R_FADING;
            }
            else
            {
                fsGDDLap(FS_GDD_STAGE_WALK);
                // the textures the stock loop binds for this batch
                U64 handle = 0;
                reason = texture_of(params.mTexture.get(), frame, handle);
                set_handle(mat, 0, handle);
                if (reason < 0 && normal_map)
                {
                    handle = 0;
                    reason = texture_of(params.mNormalMap.get(), frame, handle);
                    set_handle(mat, 1, handle);
                }
                if (reason < 0 && spec_map)
                {
                    handle = 0;
                    reason = texture_of(params.mSpecularMap.get(), frame, handle);
                    set_handle(mat, 2, handle);
                }
                fsGDDLap(FS_GDD_STAGE_TEXTURES);

                if (reason < 0)
                {
                    U32 fail = 0;
                    if ((params.mVertexBuffer->getTypeMask() & needed) != needed)
                    {   // checked before the arena, which would copy the buffer only for it to be turned down here
                        reason = R_ATTRIBUTES;
                    }
                    else if (!(entry = arena.find(params.mVertexBuffer.get(), frame, &fail)))
                    {
                        reason = R_ARENA + fail;
                    }
                    else if ((entry->mTypeMask & needed) != needed)
                    {
                        reason = R_ATTRIBUTES;
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
                ++sStatReasons[reason];
                sOps.push_back({ true, 0, 0, &params });
                if (palette)
                {
                    sStockPalettes.push_back(palette);
                }
                if (reason == R_RIGGED || reason == R_FADING || reason >= R_ARENA)
                {   // a rigged batch's avatar mustn't be held; the others may come out differently next frame
                    sKeepable = false;
                }
                continue;
            }

            // the uniforms the stock loop sets for this batch
            for (U32 c = 0; c < 4; ++c)
            {
                mat.mSpecular[c] = params.mSpecColor.mV[c];
            }
            mat.mParams[0] = params.mEnvIntensity;
            mat.mParams[1] = params.mAlphaMaskCutoff;
            mat.mParams[2] = params.mFullbright ? 1.f : 0.f;
            // consecutive batches mostly share their material
            if (sMaterials.empty() || memcmp(&sMaterials.back(), &mat, sizeof(Material)) != 0)
            {
                sMaterials.push_back(mat);
            }
            const U32 material = (U32)sMaterials.size() - 1;
            fsGDDLap(FS_GDD_STAGE_TEXTURES);
            const U32 model_matrix = sMatrices.modelIndex(params.mModelMatrix, proj, view);
            const U32 texture_matrix = sMatrices.textureIndex(params.mTextureMatrix.get());
            fsGDDLap(FS_GDD_STAGE_MATRICES);

            ++sStatDrawn;
            const U32 command = (U32)sCommands.size();
            sCommands.push_back({ params.mCount, 1, entry->mIndex + params.mOffset, (GLint)entry->mVertex, command });
            // skin SSS: the skin exclusion rides in the palette offset's top bit (assign_palettes keeps it)
            sRecords.push_back({ model_matrix, texture_matrix, material,
                                 rigged && LLRenderPass::fsSkinExcluded(params) ? FS_GDD_SKIN_EXCLUDE_BIT : 0 });
            sCommandParams.push_back(&params);
            if (palette)
            {
                sCommandPalettes.push_back(palette);
            }
            if (cache)
            {
                sArenaUses.push_back({ params.mVertexBuffer.get(), entry->mVertex, entry->mIndex });
            }

            if (grouped)
            {
                fsGDDLap(FS_GDD_STAGE_RECORDS);
                continue;
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
        if (grouped && !sCommands.empty())
        {   // the commands first, as one run
            sOps.insert(sOps.begin(), { false, 0, (U32)sCommands.size(), nullptr });
        }
        sStatMaterials += sMaterials.size();

        if (cache)
        {
            fsGDDRecordTextureUses(nullptr);
            sMatrices.record(nullptr);
            fsGDDLap(FS_GDD_STAGE_RECORDS);
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

    // Rigged pass: puts every palette the pass draws with into the palette buffer, and the offsets into the
    // records. Again if the buffer was replaced meanwhile (the first palette of a frame, or the buffer full),
    // until all are in the same storage; false if that doesn't happen.
    bool assign_palettes()
    {
        for (U32 attempt = 0; attempt < 4; ++attempt)
        {
            const U32 generation = FSSkinPaletteBuffer::generation();
            for (size_t c = 0; c < sCommandPalettes.size(); ++c)
            {
                const LLVOAvatar::MatrixPaletteCache& mpc = *sCommandPalettes[c];
                const U32 offset = FSSkinPaletteBuffer::offsetOf(mpc.mFSBuildSerial, mpc.mGLMp.data(),
                                                                 (U32)mpc.mMatrixPalette.size(), mpc.mSkinOrigin);
                if (offset == U32_MAX)
                {
                    return false;
                }
                sRecords[c].mSkin = (sRecords[c].mSkin & FS_GDD_SKIN_EXCLUDE_BIT) | (offset / 16);
            }
            // so the stock fallbacks' binds find theirs in the same storage rather than fill it up mid pass
            for (const LLVOAvatar::MatrixPaletteCache* mpc : sStockPalettes)
            {
                FSSkinPaletteBuffer::offsetOf(mpc->mFSBuildSerial, mpc->mGLMp.data(), (U32)mpc->mMatrixPalette.size(),
                                              mpc->mSkinOrigin);
            }
            if (generation == FSSkinPaletteBuffer::generation())
            {
                sPaletteGeneration = generation;
                return FSSkinPaletteBuffer::glName() != 0;
            }
        }
        return false;
    }

    // the arena, the record attribute and the storage buffers for the pass's program (bound)
    void setup_inputs(U32 index)
    {
        // the attributes go into the global vertex array object, never a buffer's own
        LLVertexBuffer::fsUseDefaultVAO();
        if (sRiggedPass)
        {   // the skinned branch of materialV.glsl projects with the projection_matrix uniform (the static one only
            // reads the records' matrices). Only gGL's sync sets it, which every stock draw goes through and a
            // multi-draw doesn't: left unset it is all zeros and every rigged vertex collapses to one point.
            gGL.syncMatrices();
        }
        if (sReplayPass >= 0)
        {   // FSRenderGPUOcclusionCulling phase 2: the copies of this pass's inputs
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATRIX_BINDING, FSGPUOcclusion::replayMatrices(sReplayPass));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATERIAL_BINDING, FSGPUOcclusion::replayMaterials(sReplayPass));
        }
        else
        {
            sMatrices.bindBase(FS_GDD_MAT_MATRIX_BINDING);
            sCurMaterials->bindBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATERIAL_BINDING);
        }
        if (sRiggedPass)
        {   // the palette buffer whole; the records hold the offsets
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_SKIN_BINDING, FSSkinPaletteBuffer::glName());
        }
        if (sIndirectOverride)
        {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, sIndirectOverride);
        }
        else
        {
            sCurIndirect->bind(GL_DRAW_INDIRECT_BUFFER);
        }
        const GLuint record = (GLuint)sRecordAttrib[index];
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
        // every attribute the program reads (bind_inputs checked the arena holds them, build that the batches have them)
        arena_for(index).bindForDraw(sPrograms[index].mAttributeMask);
    }

    // undo setup_inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    void finish_inputs(U32 index)
    {
        const GLuint record = (GLuint)sRecordAttrib[index];
        glVertexAttribDivisor(record, 0);
        glDisableVertexAttribArray(record);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATRIX_BINDING, 0);
        if (sRiggedPass)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_SKIN_BINDING, 0);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_MAT_MATERIAL_BINDING, 0);  // also unbinds the generic binding
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
        inputs.mKind = sRiggedPass ? FSGPUOcclusion::KIND_RIGGED : FSGPUOcclusion::KIND_MATERIALS;
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
        inputs.mMaterials = sCurMaterials->name();
        inputs.mMaterialsOffset = sCurMaterials->offset();
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

    // As LLDrawPoolMaterials::renderDeferred sets up before its loop. The stock program must be bound.
    void begin_stock(LLGLSLShader& stock)
    {
        sStock = StockState();
        sStock.mReady = true;
        sStock.mIntensity = stock.getUniformLocation(LLShaderMgr::ENVIRONMENT_INTENSITY);
        sStock.mBrightness = stock.getUniformLocation(LLShaderMgr::EMISSIVE_BRIGHTNESS);
        sStock.mMinAlpha = stock.getUniformLocation(LLShaderMgr::MINIMUM_ALPHA);
        sStock.mSpecular = stock.getUniformLocation(LLShaderMgr::SPECULAR_COLOR);
        sStock.mDiffuseChannel = stock.enableTexture(LLShaderMgr::DIFFUSE_MAP);
        sStock.mSpecChannel = stock.enableTexture(LLShaderMgr::SPECULAR_MAP);
        sStock.mNormChannel = stock.enableTexture(LLShaderMgr::BUMP_MAP);
        sStock.mLastSpecular = LLVector4(0, 0, 0, 0);

        gGL.getTexUnit(sStock.mDiffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);
        if (sStock.mIntensity > -1)
        {
            glUniform1f(sStock.mIntensity, sStock.mLastIntensity);
        }
        if (sStock.mBrightness > -1)
        {
            glUniform1f(sStock.mBrightness, sStock.mLastFullbright);
        }
        if (sStock.mMinAlpha > -1)
        {
            glUniform1f(sStock.mMinAlpha, sStock.mLastMinimumAlpha);
        }
        if (sStock.mSpecular > -1)
        {
            glUniform4fv(sStock.mSpecular, 1, sStock.mLastSpecular.mV);
        }
    }

    // One batch the stock way, as the loop of LLDrawPoolMaterials::renderDeferred draws it. The stock program
    // must be bound.
    void push_stock(LLGLSLShader& stock, LLDrawInfo& params)
    {
        if (!sStock.mReady)
        {
            begin_stock(stock);
        }
        StockState& s = sStock;
        if (s.mSpecular > -1 && params.mSpecColor != s.mLastSpecular)
        {
            s.mLastSpecular = params.mSpecColor;
            glUniform4fv(s.mSpecular, 1, s.mLastSpecular.mV);
        }
        if (s.mIntensity > -1 && s.mLastIntensity != params.mEnvIntensity)
        {
            s.mLastIntensity = params.mEnvIntensity;
            glUniform1f(s.mIntensity, s.mLastIntensity);
        }
        if (s.mMinAlpha > -1 && s.mLastMinimumAlpha != params.mAlphaMaskCutoff)
        {
            s.mLastMinimumAlpha = params.mAlphaMaskCutoff;
            glUniform1f(s.mMinAlpha, s.mLastMinimumAlpha);
        }
        const F32 fullbright = params.mFullbright ? 1.f : 0.f;
        if (s.mBrightness > -1 && s.mLastFullbright != fullbright)
        {
            s.mLastFullbright = fullbright;
            glUniform1f(s.mBrightness, s.mLastFullbright);
        }
        if (s.mNormChannel > -1 && params.mNormalMap != s.mLastNormalMap)
        {
            s.mLastNormalMap = params.mNormalMap;
            if (s.mLastNormalMap)
            {
                gGL.getTexUnit(s.mNormChannel)->bindFast(s.mLastNormalMap);
            }
        }
        if (s.mSpecChannel > -1 && params.mSpecularMap != s.mLastSpecMap)
        {
            s.mLastSpecMap = params.mSpecularMap;
            if (s.mLastSpecMap)
            {
                gGL.getTexUnit(s.mSpecChannel)->bindFast(s.mLastSpecMap);
            }
        }
        if (params.mTexture != s.mLastDiffuse)
        {
            s.mLastDiffuse = params.mTexture;
            if (s.mLastDiffuse)
            {
                gGL.getTexUnit(s.mDiffuseChannel)->bindFast(s.mLastDiffuse);
            }
            else
            {
                gGL.getTexUnit(s.mDiffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }

        // rigged: its palette's range (build put the batch here only with one, and the palette in the buffer)
        if (sRiggedPass && !LLRenderPass::uploadMatrixPalette(params.mAvatar, params.mSkinInfo.get()))
        {
            return;
        }

        LLRenderPass::applyModelMatrix(params);

        bool tex_setup = false;
        if (params.mTextureMatrix)
        {
            gGL.getTexUnit(0)->activate();
            gGL.matrixMode(LLRender::MM_TEXTURE);
            gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;
            tex_setup = true;
        }

        params.mVertexBuffer->setBuffer();
        params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount, params.mOffset);

        if (tex_setup)
        {
            gGL.getTexUnit(0)->activate();
            gGL.loadIdentity();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
        }
    }

    // Draws the ops. The stock program is bound on entry and on return. False if the GPU-driven program
    // couldn't be bound before anything was drawn (then nothing was). cull: FSRenderGPUOcclusionCulling phase 1
    // for this pass (occ: its occlusion index, or -1 when it wasn't culled).
    bool execute(U32 index, LLGLSLShader& stock, U32& calls, U32& switches, bool cull, S32& occ)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials execute");
        LL_PROFILE_GPU_ZONE("gdd materials");
        // the rigged pass apart: zone "gbuffer rigged" of the FSGPUTimings log, inside the pass's zone
        std::optional<LLFSGPUZone::Scope> rigged_zone;
        if (sRiggedPass)
        {
            rigged_zone.emplace("gbuffer rigged");
        }

        // a kept pass used again (FSPerfGPUDrivenBuildCache) has its commands, records and materials up already
        if (sUploads & UPLOAD_COMMANDS)
        {
            sCurIndirect->upload(GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(FSGDDDrawCommand));
        }
        if (sUploads & UPLOAD_RECORDS)
        {
            sCurRecords->upload(GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        }
        sMatrices.upload();
        if (sUploads & UPLOAD_MATERIALS)
        {
            sCurMaterials->upload(GL_SHADER_STORAGE_BUFFER, sMaterials.data(), sMaterials.size() * sizeof(Material));
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
                    finish_inputs(index);
                    stock.bind();
                    gdd = false;
                    ++switches;
                }
                push_stock(stock, *op.mParams);
                drew = true;
                continue;
            }

            if (sRiggedPass && FSSkinPaletteBuffer::generation() != sPaletteGeneration)
            {   // the palette buffer was replaced after the offsets were taken (not expected: assign_palettes put
                // every palette of the pass in it); the run the stock way, which binds each palette again
                LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenRigged: the palette buffer was replaced mid pass" << LL_ENDL;
                if (op.mCount)
                {
                    mark_stock(range);
                }
                if (gdd)
                {
                    finish_inputs(index);
                    stock.bind();
                    gdd = false;
                    ++switches;
                }
                for (U32 c = op.mFirst; c < op.mFirst + op.mCount; ++c)
                {
                    push_stock(stock, *sCommandParams[c]);
                }
                drew = true;
                continue;
            }

            if (!gdd)
            {
                if (!bind_program(index))
                {
                    stock.bind();
                    if (!drew)
                    {
                        return false;
                    }
                    // not expected (fsGDDDrawMaterials bound the program once already); the rest of the pass goes
                    // the stock way, in order
                    LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenMaterials: a program failed to bind mid pass" << LL_ENDL;
                    U32 rest_range = op.mCount ? range : next_range;   // this op's range, or the next one
                    for (size_t i = &op - sOps.data(); i < sOps.size(); ++i)
                    {
                        const Op& rest = sOps[i];
                        if (rest.mStock)
                        {
                            push_stock(stock, *rest.mParams);
                        }
                        else
                        {
                            if (rest.mCount)
                            {
                                mark_stock(rest_range++);
                            }
                            for (U32 c = rest.mFirst; c < rest.mFirst + rest.mCount; ++c)
                            {
                                push_stock(stock, *sCommandParams[c]);
                            }
                        }
                    }
                    return true;
                }
                ++switches;
                gdd = true;
                setup_inputs(index);
            }
            multi_draw(op, range);
            ++calls;
            drew = true;
        }
        if (gdd)
        {
            finish_inputs(index);
            stock.bind();
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
        const U32 index = replay.mIndex;
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials phase 2");
        LL_PROFILE_GPU_ZONE("gdd materials phase 2");
        if (replay.mRigged && FSSkinPaletteBuffer::generation() != replay.mGeneration &&
            !FSGPUOcclusion::refreshPalettes(occ, replay.mParams, replay.mRecords, sizeof(Record) / sizeof(U32), 3,
                                             replay.mGeneration))
        {   // not expected: place() found no buffer, or it never held still
            LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: the joint palettes of a rigged material pass could not be "
                                       << "put in the palette buffer again for its phase 2" << LL_ENDL;
            return;
        }
        sRiggedPass = is_rigged(index);
        std::optional<LLFSGPUZone::Scope> rigged_zone;
        if (sRiggedPass)
        {
            rigged_zone.emplace("gbuffer rigged");
        }
        sReplayPass = occ;
        sIndirectOverride = FSGPUOcclusion::replayCommands(occ);
        LLVertexBuffer::unbind();
        LLGLDepthTest depth(GL_TRUE, GL_TRUE);
        LLGLDisable blend(GL_BLEND);

        sPrograms[index].mUniformsDirty = true;
        if (!bind_program(index))
        {   // not expected: it drew in phase 1 this frame
            LL_WARNS_ONCE("Rendering") << "FSRenderGPUOcclusionCulling: a material program failed to bind in phase 2" << LL_ENDL;
        }
        else
        {
            setup_inputs(index);
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
            finish_inputs(index);
            sStatCalls += calls;
        }
        sReplayPass = -1;
        sIndirectOverride = 0;
        STOP_GLERROR;
    }

    // Keeps the runs of a pass phase 1 culled, for its phase 2
    void keep_for_phase_two(S32 occ, U32 index)
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
        replay.mIndex = index;
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

    // FSPerfGPUDrivenMaterialsVerify: the batches drawn from the arena again, the stock way, colour and depth
    // writes off, depth test GL_LESS against what the multi-draws left. The stock program must be bound.
    void verify_against_stock(LLGLSLShader& stock)
    {
        if (sCommandParams.empty())
        {
            return;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials verify");

        static LLFrameTimer error_timer;
        fsGDDCheckGLError(error_timer, "FSPerfGPUDrivenMaterialsVerify", stock.mName);

        // arena copies against their sources, a couple per pass
        sStatMismatches += fsGDDVerifyArenaCopies(*sVerifyArena, sCommandParams,
                                                  [](LLDrawInfo* params) { return params->mVertexBuffer.get(); },
                                                  "FSPerfGPUDrivenMaterialsVerify");

        // the occlusion check needs the multi-draws' depth in the buffer
        GLboolean depth_test = GL_FALSE;
        GLboolean depth_write = GL_FALSE;
        glGetBooleanv(GL_DEPTH_TEST, &depth_test);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
        if (!depth_test || !depth_write)
        {
            return;
        }

        const GLuint samples = fsGDDNearerSamples(sQuery, false, [&]()
        {
            for (LLDrawInfo* params : sCommandParams)
            {
                push_stock(stock, *params);
            }
        });
        if (samples)
        {
            ++sStatMismatches;
            static LLFrameTimer warn_timer;
            if (fsGDDWarnDue(warn_timer))
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenMaterialsVerify: " << samples << " samples of " << sCommandParams.size()
                                      << " stock batches (" << stock.mName << ") are nearer than the multi-draw depth; "
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
        LL_INFOS("FSGPUDrivenMaterials") << "GPU-driven materials, last 10 s: " << sStatPasses << " passes, " << sStatBatches
                                         << " batches (" << sStatDrawn << " from the arena, " << sStatMaterials
                                         << " material records) in " << sStatCalls << " multi-draw calls, " << sStatSwitches
                                         << " program switches; stock path for: "
                                         << reasons << "; arena "
                                         << fsGDDArenaSummary(sArena) << "; rigged arena "
                                         << fsGDDArenaSummary(sRiggedArena) << "; "
                                         << sStatMismatches << " verify mismatches" << LL_ENDL;
        sStatPasses = sStatBatches = sStatDrawn = sStatCalls = sStatSwitches = sStatMaterials = 0;
        sArena.clearCounts();
        sRiggedArena.clearCounts();
    }
}

bool fsGDDDrawMaterials(U32 type, LLGLSLShader& stock)
{
    // idle handles go non resident and away also while the other bindless passes are off (it does something
    // every 120 frames)
    static U32 s_swept_frame = U32_MAX;
    const U32 sweep_frame = LLFrameTimer::getFrameCount();
    if (sweep_frame != s_swept_frame)
    {
        s_swept_frame = sweep_frame;
        fsGDDSweepHandles(sweep_frame);
    }

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenMaterials", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !fsGDDBindlessReady() || LLPipeline::sShadowRender ||
        LLPipeline::sRenderingHUDs)
    {
        return false;
    }
    // the non blended permutations only, rigged ones with FSPerfGPUDrivenRigged (not for impostors)
    U32 index = 0;
    while (index < FS_GDD_MAT_PROGRAMS && &gDeferredMaterialProgram[index] != &stock)
    {
        ++index;
    }
    const bool rigged_ok = rigged_enabled() && !LLPipeline::sImpostorRender;
    if (!wanted_program(index, rigged_ok) || LLGLSLShader::sCurBoundShaderPtr != &stock || !program_usable(index))
    {
        return false;
    }
    sRiggedPass = is_rigged(index);
    sVerifyArena = &arena_for(index);
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);
    if (begin == end)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfGPUDrivenMaterialsVerify", false);
    static LLCachedControl<bool> stats(gSavedSettings, "FSPerfGPUDrivenMaterialsStats", false);
    static LLCachedControl<bool> group_runs(gSavedSettings, "FSPerfGPUDrivenPBRGroupRuns", false);

    // The first bind finds the program's inputs; do that now, so a failure leaves the whole pass to the
    // stock loop rather than part of it
    if (!sInputsReady[index])
    {
        const bool ready = bind_program(index);
        stock.bind();
        if (!ready)
        {
            return false;
        }
    }

    // The environment uniforms (LLEnvironment marks the programs of the shader list dirty when they change,
    // and the stock program took them when the pool bound it); ours isn't in that list, so it takes the
    // current values at its first bind in each pass
    sPrograms[index].mUniformsDirty = true;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd materials build");
        // FSPerfGPUDrivenBuildCache: not while verifying, which checks what a build makes
        const bool cache = !verify && FSGDDBuildCache::enabled();
        build(index, begin, end, group_runs, cache, ((U64)type << 32) | ((U64)index << 1) | (group_runs ? 1 : 0));
    }
    if (sRiggedPass && !sCommands.empty() && !assign_palettes())
    {   // no buffer, or it never held still: the stock loop draws the pass
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenRigged: the joint palettes could not be put in the palette buffer; "
                                   << "a rigged pass is drawn as before" << LL_ENDL;
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

    sStock.mReady = false;  // the stock loop's state, set up at the first batch drawn the stock way
    U32 calls = 0;
    U32 switches = 0;
    // FSRenderGPUOcclusionCulling: the main camera's G-buffer passes; not while verifying, which compares
    // against everything the stock path draws
    const bool cull = !verify && FSGPUOcclusion::passActive();
    S32 occ = -1;
    if (!execute(index, stock, calls, switches, cull, occ))
    {
        return false;
    }
    if (occ >= 0)
    {
        keep_for_phase_two(occ, index);
    }

    if (verify)
    {
        verify_against_stock(stock);
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
