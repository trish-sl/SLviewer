/**
 * @file fsgpudrivenalpha.cpp
 * @brief GPU-driven draws for the forward alpha pass (FSPerfGPUDrivenAlpha)
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

// The forward alpha pass (LLDrawPoolAlpha::renderAlpha) draws the alpha groups back to front, and within a
// group its draws in their sorted order, one at a time: per draw a program switch (it goes back and forth
// between the legacy alpha, fullbright alpha, material and rigged programs), the specular colour,
// environment intensity and brightness uniforms, up to 16 texture binds (or one and a texture matrix), the
// model matrix, a joint palette range for rigged ones, the blend function, a vertex buffer bind and a
// glDrawRangeElements: about 11 GL calls a draw, 500 to 2000 draws a frame in a crowd.
//
// With FSPerfGPUDrivenAlpha on, runs of consecutive draws of a group that share their program and plain
// source-over blending are drawn as one glMultiDrawElementsIndirect each, out of a vertex arena (fsgddarena.h),
// in the stock place in the loop. A multi-draw keeps the order of its commands and of their primitives, and
// nothing else is drawn in between, so the blending is exactly the stock blending. Every command has a record
// (an instanced attribute its base instance selects): its matrices (a storage buffer of the matrices the stock
// path would have computed, computed the same way), its texture matrix, and its textures and material:
// - the legacy alpha and fullbright alpha draws (gDeferredAlphaProgram, gDeferredFullbrightAlphaMaskAlphaProgram)
//   as in FSPerfGPUDrivenLegacy (fsgpudrivenlegacy.cpp): a header with the pass's minimum alpha and the texture
//   count, then the bindless handle of every texture LLDrawPoolAlpha::TexSetup binds, in unit order, which the
//   vertex shader (alphaV.glsl, fullbrightV.glsl built with FS_GDD_LEGACY) picks by the vertex's texture index
//   as diffuseLookup picks a unit;
// - the legacy material draws in blend mode (gDeferredMaterialProgram[mask | DIFFUSE_ALPHA_MODE_BLEND]) as in
//   FSPerfGPUDrivenMaterials (fsgpudrivenmaterials.cpp): the specular colour, environment intensity and
//   brightness the stock loop sets, and the handles of the diffuse, normal and specular maps
//   (materialV.glsl / materialF.glsl built with FS_GDD_MATERIAL).
// Rigged draws (FSPerfGPUDrivenRigged, FSPerfRiggedPaletteBuffer) come from a second arena that also holds the
// joint weights; the record's w names where the draw's palette starts in this frame's palette buffer, bound
// whole as a storage buffer, as in the opaque rigged multi-draws.
//
// Everything else is the stock code: the programs are the stock programs with only the per draw inputs
// replaced, bound the way the stock loop binds its programs (the caller's bind: bindDeferredShaderFast, the
// pass uniforms once), prepared as it prepares them (prepare_alpha_shader: gamma, water plane, minimum alpha,
// fog), and gGL.syncMatrices runs before every multi-draw as before every stock draw: it sets the light
// uniforms the forward lighting reads and the projection the skinned branches read. Per draw the stock loop
// still applies the model matrix (the group's glow pass draws with the last one), keeps the lighting state it
// tracks, and collects the glow draws, which are drawn after the group as before.
//
// Drawn the stock way, in place: PBR (glTF) draws; custom blend functions; draws fading in (FSRenderFadeIn);
// a texture missing or without a handle, a texture list on a material draw, a normal or specular map the
// program samples but the draw doesn't have; material draws not in blend mode; empty draws, 32 bit indices,
// buffers mapped or lacking an attribute the program reads, arena full or the copy budget used up; and runs
// shorter than FS_GDD_ALPHA_MIN_RUN, which would cost more than they save. The whole pass is stock for HUDs,
// impostors, reflection probes, the depth only passes (depth of field, WBOIT depth, hair depth), the WBOIT
// accumulate and residual phases and the two-pass hair passes (LLDrawPoolAlpha::renderAlpha decides).
//
// One upload per group (commands, records, matrices, textures, materials), all palettes of the group placed in
// the palette buffer before its loop (FSSkinPaletteBuffer::place, the stock rigged draws' too); should the
// buffer still be replaced mid group, the palettes are placed again and the records uploaded again before the
// next run.

#include "llviewerprecompiledheaders.h"

#include "fsgpudrivenalpha.h"
#include "fsreversez.h"

#include "fsgddarena.h"
#include "fsgddcommon.h"
#include "fsgpudrivenlegacy.h"
#include "fsgpudrivenmaterials.h"
#include "fsgpudrivenpbr.h"
#include "fsskinpalettebuffer.h"

#include "lldrawpool.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llmaterial.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    // the other GPU-driven passes use the same bindings; each binds its buffers and clears them again
    constexpr GLuint FS_GDD_ALPHA_MATRIX_BINDING = 5;   // FSGDDLegacyMatrices / FSGDDMaterialMatrices
    constexpr GLuint FS_GDD_ALPHA_DATA_BINDING = 6;     // FSGDDLegacyTextures / FSGDDMaterialRecords
    constexpr GLuint FS_GDD_ALPHA_SKIN_BINDING = 7;     // FSGDDSkinPalettes (objectSkinV.glsl, rigged)
    // A run this short goes the stock way: its program bind, inputs and multi-draw cost about as much as the
    // stock draws they replace
    constexpr U32 FS_GDD_ALPHA_MIN_RUN = 3;
    constexpr U32 FS_GDD_ALPHA_MAX_TEXTURES = 16;       // a legacy draw's texture list, as many as TexSetup binds
    // the attributes the arenas can feed; a program wanting any other is not used (the weights only the rigged
    // arena has; the texture index is the w of the positions)
    constexpr U32 FS_GDD_ALPHA_ATTRIBS = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TEXCOORD0 |
                                         LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 |
                                         LLVertexBuffer::MAP_TEXCOORD2 | LLVertexBuffer::MAP_TEXTURE_INDEX |
                                         LLVertexBuffer::MAP_WEIGHT4;

    // Everything the alpha, fullbright and material vertex shaders read; normals (fullbright buffers have
    // none), tangents and the second and third texture coordinates only where the buffer has them (a program
    // that reads them turns a buffer without them down). The size limit of FSPerfGPUDrivenMaterials', each.
    FSGDDArena sArena("FSPerfGPUDrivenAlpha arena",
                      { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                        LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_TANGENT, LLVertexBuffer::TYPE_TEXCOORD1,
                        LLVertexBuffer::TYPE_TEXCOORD2 },
                      "FSPerfGPUDrivenMaterialsArenaMB", 256, 8,
                      LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 |
                          LLVertexBuffer::MAP_TEXCOORD2);
    // the same plus the joint weights, for the rigged pass
    FSGDDArena sRiggedArena("FSPerfGPUDrivenAlpha rigged arena",
                            { LLVertexBuffer::TYPE_VERTEX, LLVertexBuffer::TYPE_NORMAL, LLVertexBuffer::TYPE_TEXCOORD0,
                              LLVertexBuffer::TYPE_COLOR, LLVertexBuffer::TYPE_TANGENT, LLVertexBuffer::TYPE_TEXCOORD1,
                              LLVertexBuffer::TYPE_TEXCOORD2, LLVertexBuffer::TYPE_WEIGHT4 },
                            "FSPerfGPUDrivenMaterialsArenaMB", 256, 8,
                            LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_TANGENT | LLVertexBuffer::MAP_TEXCOORD1 |
                                LLVertexBuffer::MAP_TEXCOORD2);

    struct Record       // fs_gdd_draw
    {
        U32 mMatrix;    // modelview; modelview projection and normal matrix follow
        U32 mTexMatrix;
        U32 mData;      // legacy: first entry in the texture buffer (the header); material: the material
        U32 mSkin;      // rigged: the palette's first vec4 in the palette buffer (FSSkinPaletteBuffer::place)
    };

    static_assert(sizeof(FSGDDTexEntry) == 8, "FSGDDLegacyTextures layout");

    struct Material     // FSGDDLegacyMaterial, std430 (FSGDDMaterialRecords)
    {
        F32 mSpecular[4];   // specular_color
        F32 mParams[4];     // env_intensity, minimum_alpha, emissive_brightness, 0
        U32 mTex[8];        // handles (low, high): diffuseMap, bumpMap, specularMap, unused
    };
    static_assert(sizeof(Material) == 64, "FSGDDLegacyMaterial layout");

    // The program families; a slot is a family, plus F_COUNT for its skinned program
    enum EFamily
    {
        F_ALPHA = 0,        // gDeferredAlphaProgram
        F_FULLBRIGHT,       // gDeferredFullbrightAlphaMaskAlphaProgram
        F_MATERIAL,         // gDeferredMaterialProgram[(k << 2) | DIFFUSE_ALPHA_MODE_BLEND], k = 0..3: F_MATERIAL + k
        F_COUNT = F_MATERIAL + 4
    };
    constexpr U32 SLOT_COUNT = F_COUNT * 2;

    LLGLSLShader sPrograms[SLOT_COUNT];
    GLint sRecordAttrib[SLOT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    S32 sInputsReady[SLOT_COUNT] = {};      // bind_inputs done: 0 not yet, 1 yes, -1 failed

    bool slot_rigged(U32 slot)
    {
        return slot >= F_COUNT;
    }

    U32 slot_family(U32 slot)
    {
        return slot % F_COUNT;
    }

    bool slot_material(U32 slot)
    {
        return slot_family(slot) >= F_MATERIAL;
    }

    // the stock program of a slot, nullptr if there is none
    LLGLSLShader* stock_of(U32 slot)
    {
        const U32 family = slot_family(slot);
        LLGLSLShader* stock = nullptr;
        if (family == F_ALPHA)
        {
            stock = &gDeferredAlphaProgram;
        }
        else if (family == F_FULLBRIGHT)
        {
            stock = &gDeferredFullbrightAlphaMaskAlphaProgram;
        }
        else
        {
            stock = &gDeferredMaterialProgram[((family - F_MATERIAL) << 2) | LLMaterial::DIFFUSE_ALPHA_MODE_BLEND];
        }
        if (slot_rigged(slot))
        {
            stock = stock->mRiggedVariant != stock ? stock->mRiggedVariant : nullptr;
        }
        return stock;
    }

    const char* slot_name(U32 slot)
    {
        static const char* names[SLOT_COUNT] =
        {
            "FS GPU-Driven Alpha Shader", "FS GPU-Driven Fullbright Alpha Shader",
            "FS GPU-Driven Alpha Material Shader 1", "FS GPU-Driven Alpha Material Shader 5",
            "FS GPU-Driven Alpha Material Shader 9", "FS GPU-Driven Alpha Material Shader 13",
            "FS GPU-Driven Skinned Alpha Shader", "FS GPU-Driven Skinned Fullbright Alpha Shader",
            "FS GPU-Driven Skinned Alpha Material Shader 1", "FS GPU-Driven Skinned Alpha Material Shader 5",
            "FS GPU-Driven Skinned Alpha Material Shader 9", "FS GPU-Driven Skinned Alpha Material Shader 13",
        };
        return names[slot];
    }

    FSGDDBuffer sIndirectBuffer;
    FSGDDBuffer sRecordBuffer;
    FSGDDBuffer sTextureBuffer;
    FSGDDBuffer sMaterialBuffer;

    // the pass
    bool sPassOn = false;
    bool sPassRigged = false;
    LLRender::eBlendFactor sAlphaS = LLRender::BF_ZERO;
    LLRender::eBlendFactor sAlphaD = LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
    F32 sMinimumAlpha = 0.f;

    // the group: a candidate per draw (phase one), then the commands of the runs kept (phase two)
    struct Candidate
    {
        LLDrawInfo* mParams;
        const FSGDDArenaEntry* mEntry;
        S32 mSlot;              // -1: the stock way
        U32 mFirstHandle;       // in sHandles
        U32 mHandleCount;       // legacy: the textures; material: 3 (diffuse, normal, specular; 0 where unused)
        bool mTexMatrix;        // legacy: the single texture's matrix applies
    };
    std::vector<Candidate> sCandidates;     // by the draw's index in the group
    std::vector<U64> sHandles;
    std::vector<S32> sDrawCommand;          // by the draw's index in the group
    std::vector<FSGDDDrawCommand> sCommands;
    std::vector<Record> sRecords;
    std::vector<U32> sCommandSlot;
    std::vector<LLDrawInfo*> sCommandParams;
    FSGDDMatrixTable sMatrices;
    std::vector<FSGDDTexEntry> sTextures;
    std::vector<Material> sMaterials;
    // rigged: the group's draws drawn the stock way (their palettes go into the buffer too), the offsets and
    // the generation they hold for
    std::vector<LLDrawInfo*> sStockRigged;
    std::vector<U32> sPaletteOffsets;
    U32 sPaletteGeneration = 0;

    // the run waiting to be drawn
    S32 sPendingSlot = -1;
    U32 sPendingFirst = 0;
    U32 sPendingCount = 0;
}

//-----------------------------------------------------------------------------
// Programs

namespace
{
    bool create_program(U32 slot)
    {
        const LLGLSLShader& stock = *stock_of(slot);
        LLGLSLShader& shader = sPrograms[slot];
        shader.mName = slot_name(slot);
        shader.mFeatures = stock.mFeatures;
        // no indexed texture units: the vertex shader reads texture_index itself and diffuseLookup samples a handle
        shader.mFeatures.mIndexedTextureChannels = 0;
        // As the stock programs were built (LLViewerShaderMgr::loadShadersDeferred sets these after building
        // them, so the lighting objects aren't linked); put back below, they decide whether gGL syncs the lights
        if (slot_family(slot) == F_ALPHA)
        {
            shader.mFeatures.calculatesLighting = false;
            shader.mFeatures.hasLighting = false;
        }
        else if (slot_material(slot))
        {
            shader.mFeatures.hasLighting = false;
        }
        // rigged (HAS_SKIN among the stock defines): the skinning object that reads the palettes from the storage buffer
        shader.mFeatures.mFSGDDSkinning = slot_rigged(slot);
        shader.mShaderFiles = stock.mShaderFiles;
        shader.mShaderLevel = stock.mShaderLevel;
        shader.mShaderGroup = stock.mShaderGroup;
        shader.clearPermutations();
        shader.mDefines = stock.mDefines;   // the stock permutations (HAS_SKIN, FS_WBOIT, FS_HAIR, ...), then ours
        shader.addPermutation(slot_material(slot) ? "FS_GDD_MATERIAL" : "FS_GDD_LEGACY", "1");
        const bool success = shader.createShader();
        shader.mUniformsDirty = true; // the environment uniforms at its first bind
        shader.mFeatures.calculatesLighting = stock.mFeatures.calculatesLighting;
        shader.mFeatures.hasLighting = stock.mFeatures.hasLighting;
        return success;
    }

    // the storage block bindings, where the driver put the per command record, and whether the arenas can
    // feed every attribute the program reads
    bool bind_inputs(U32 slot)
    {
        LLGLSLShader& shader = sPrograms[slot];
        const bool material = slot_material(slot);
        const bool rigged = slot_rigged(slot);
        const GLuint matrices = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK,
                                                          material ? "FSGDDMaterialMatrices" : "FSGDDLegacyMatrices");
        const GLuint data = glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK,
                                                      material ? "FSGDDMaterialRecords" : "FSGDDLegacyTextures");
        const GLuint palettes = rigged ? glGetProgramResourceIndex(shader.mProgramObject, GL_SHADER_STORAGE_BLOCK, "FSGDDSkinPalettes")
                                       : GL_INVALID_INDEX;
        sRecordAttrib[slot] = glGetAttribLocation(shader.mProgramObject, "fs_gdd_draw");
        const U32 mask = shader.mAttributeMask;
        if (matrices == GL_INVALID_INDEX || data == GL_INVALID_INDEX || (rigged && palettes == GL_INVALID_INDEX) ||
            sRecordAttrib[slot] < 0 || (mask & ~FS_GDD_ALPHA_ATTRIBS) || !(mask & LLVertexBuffer::MAP_VERTEX) ||
            (!material && !(mask & LLVertexBuffer::MAP_TEXTURE_INDEX)) ||
            (rigged != ((mask & LLVertexBuffer::MAP_WEIGHT4) != 0)))
        {
            return false;
        }
        glShaderStorageBlockBinding(shader.mProgramObject, matrices, FS_GDD_ALPHA_MATRIX_BINDING);
        glShaderStorageBlockBinding(shader.mProgramObject, data, FS_GDD_ALPHA_DATA_BINDING);
        if (rigged)
        {
            glShaderStorageBlockBinding(shader.mProgramObject, palettes, FS_GDD_ALPHA_SKIN_BINDING);
        }
        return true;
    }

    // Complete, and (the first time, without binding it) its inputs found
    bool program_usable(U32 slot)
    {
        if (!sPrograms[slot].isComplete())
        {
            return false;
        }
        if (!sInputsReady[slot])
        {
            sInputsReady[slot] = bind_inputs(slot) ? 1 : -1;
            if (sInputsReady[slot] < 0)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenAlpha: " << sPrograms[slot].mName << " lacks its storage blocks or "
                                      << "draw record attribute, or reads an attribute the arena doesn't hold; its draws "
                                      << "are drawn as before" << LL_ENDL;
            }
        }
        return sInputsReady[slot] > 0;
    }
}

void fsGDDAlphaUnloadShaders()
{
    for (U32 slot = 0; slot < SLOT_COUNT; ++slot)
    {
        sPrograms[slot].unload();
        sInputsReady[slot] = 0;
        sRecordAttrib[slot] = -1;
    }
    fsGDDAlphaReleaseGL();
}

void fsGDDAlphaLoadShaders()
{
    fsGDDAlphaUnloadShaders();

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenAlpha", false);
    if (!enabled)
    {
        return;
    }
    if (!gGLManager.mFSHasGPUDrivenDraws)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenAlpha: needs OpenGL 4.3 with GL_ARB_multi_draw_indirect and "
                                       << "GL_ARB_shader_storage_buffer_object; alpha draws are drawn as before" << LL_ENDL;
        return;
    }
    GLint vs_blocks = 0;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vs_blocks);
    if (vs_blocks < 2)
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenAlpha: needs two shader storage blocks in the vertex shader, "
                                       << "the driver offers " << vs_blocks << "; alpha draws are drawn as before" << LL_ENDL;
        return;
    }
    // every draw has its own textures; binding them is much of what this saves
    if (!fsGDDBindlessSetup())
    {
        LL_INFOS_ONCE("ShaderLoading") << "FSPerfGPUDrivenAlpha: needs bindless textures (NVIDIA GPUs only); "
                                       << "alpha draws are drawn as before" << LL_ENDL;
        return;
    }

    // FSPerfGPUDrivenRigged: the skinned programs too (optional: without them rigged alpha stays stock)
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
            LL_WARNS("ShaderLoading") << "FSPerfGPUDrivenAlpha: " << sPrograms[slot].mName << " failed to build, "
                                      << "its draws are drawn as before" << LL_ENDL;
            sPrograms[slot].unload();
        }
    }
}

void fsGDDAlphaReleaseGL()
{
    sArena.releaseGL();
    sRiggedArena.releaseGL();
    sIndirectBuffer.release();
    sRecordBuffer.release();
    sTextureBuffer.release();
    sMaterialBuffer.release();
    sMatrices.release();
    sPassOn = false;
    sPendingSlot = -1;
    sPendingCount = 0;
}

void fsGDDAlphaPrograms(std::vector<LLGLSLShader*>& programs)
{
    for (LLGLSLShader& shader : sPrograms)
    {
        if (shader.isComplete())
        {
            programs.push_back(&shader);
        }
    }
}

void fsGDDAlphaMarkUniformsDirty()
{
    // the environment uniforms: LLEnvironment marks the programs of the shader list dirty when it updates
    // them, and ours (not in that list) with them, so they take the current values at their next bind
    for (LLGLSLShader& shader : sPrograms)
    {
        shader.mUniformsDirty = true;
    }
}

//-----------------------------------------------------------------------------
// The pass

bool fsGDDAlphaBeginPass(bool rigged, LLGLSLShader* simple, LLGLSLShader* fullbright, LLRender::eBlendFactor alpha_s,
                         LLRender::eBlendFactor alpha_d, F32 minimum_alpha)
{
    sPassOn = false;
    sPendingSlot = -1;
    sPendingCount = 0;

    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenAlpha", false);
    if (!enabled || !gGLManager.mFSHasGPUDrivenDraws || !fsGDDBindlessReady() || LLPipeline::sShadowRender ||
        LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender)
    {
        return false;
    }
    // the programs the stock loop picks must be the ones ours are copies of
    if (simple != &gDeferredAlphaProgram || fullbright != &gDeferredFullbrightAlphaMaskAlphaProgram)
    {
        return false;
    }
    if (rigged && !fsGDDRiggedEnabled())
    {
        return false;
    }
    bool any = false;
    for (U32 slot = rigged ? F_COUNT : 0; slot < (rigged ? SLOT_COUNT : (U32)F_COUNT); ++slot)
    {
        any = any || sPrograms[slot].isComplete();
    }
    if (!any)
    {
        return false;
    }

    sPassOn = true;
    sPassRigged = rigged;
    sAlphaS = alpha_s;
    sAlphaD = alpha_d;
    sMinimumAlpha = minimum_alpha;
    return true;
}

void fsGDDAlphaEndPass()
{
    sPassOn = false;
    sPendingSlot = -1;
    sPendingCount = 0;
}

namespace
{
    // The bindless handle of a texture the stock loop binds, or 0
    U64 handle_of(LLViewerTexture* tex, U32 frame)
    {
        return tex ? fsGDDPassTextureHandle(tex, frame) : 0;
    }

    // Phase one: which program (slot) a draw goes to and what it needs, or why not (mSlot -1)
    void classify(Candidate& c, U32 frame, FSGDDArena& arena)
    {
        LLDrawInfo& params = *c.mParams;
        c.mSlot = -1;
        c.mFirstHandle = (U32)sHandles.size();
        c.mHandleCount = 0;
        c.mTexMatrix = false;
        c.mEntry = nullptr;

        // PBR (the stock loop's glTF branch, also for glTF materials not in blend mode, whose TexSetup differs),
        // empty draws, custom blend functions (they also reset the minimum alpha) and draws fading in (their
        // fs_fade_hide is set per draw) stay stock
        if (params.mGLTFMaterial.notNull() || !params.mCount || params.mVertexBuffer.isNull() ||
            params.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA || params.mBlendFuncDst != LLRender::BF_ONE_MINUS_SOURCE_ALPHA ||
            LLRenderPass::fsFadeInHidden(params) != 0.f) // fading in, or out (FSRenderFadeLOD)
        {
            return;
        }

        // the program the stock loop picks (renderAlpha: material, else simple or fullbright; HUDs excluded)
        U32 family = 0;
        LLMaterial* mat = params.mMaterial.get();
        if (mat)
        {
            const U32 mask = params.mShaderMask;
            if (mask >= LLMaterial::SHADER_COUNT || (mask & 0x3) != LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
            {
                return;
            }
            family = F_MATERIAL + (mask >> 2);
        }
        else
        {
            family = params.mFullbright ? F_FULLBRIGHT : F_ALPHA;
        }
        const U32 slot = family + (sPassRigged ? F_COUNT : 0);
        const LLGLSLShader* stock = stock_of(slot);
        if (!stock || !program_usable(slot))
        {
            return;
        }

        // the textures TexSetup binds
        if (mat)
        {
            // material: the diffuse map on its channel (a texture list would go on units 0..n instead), and the
            // normal and specular maps where the program samples them and the draw has them (else the stock
            // loop leaves an earlier draw's on the unit)
            if (params.mTextureList.size() > 1)
            {
                return;
            }
            const bool normal_map = stock->getTextureChannel(LLShaderMgr::BUMP_MAP) > -1;
            const bool spec_map = stock->getTextureChannel(LLShaderMgr::SPECULAR_MAP) > -1;
            const U64 diffuse = handle_of(params.mTexture.get(), frame);
            const U64 normal = normal_map ? handle_of(params.mNormalMap.get(), frame) : 0;
            const U64 spec = spec_map ? handle_of(params.mSpecularMap.get(), frame) : 0;
            if (!diffuse || (normal_map && !normal) || (spec_map && !spec))
            {
                return;
            }
            sHandles.push_back(diffuse);
            sHandles.push_back(normal);
            sHandles.push_back(spec);
            c.mHandleCount = 3;
            c.mTexMatrix = true;
        }
        else if (params.mTextureList.size() > 1)
        {
            // every texture of the list on its unit, the texture matrix untouched (identity); picked by the
            // vertex's texture index, which buffers without it don't have
            if (params.mTextureList.size() > FS_GDD_ALPHA_MAX_TEXTURES ||
                !(params.mVertexBuffer->getTypeMask() & LLVertexBuffer::MAP_TEXTURE_INDEX))
            {
                return;
            }
            for (const LLPointer<LLViewerTexture>& tex : params.mTextureList)
            {
                const U64 handle = handle_of(tex.get(), frame);
                if (!handle)
                {   // a missing one keeps an earlier draw's texture on its unit on the stock path
                    sHandles.resize(c.mFirstHandle);
                    return;
                }
                sHandles.push_back(handle);
            }
            c.mHandleCount = (U32)params.mTextureList.size();
        }
        else
        {
            // one texture on unit 0, and the texture matrix if there is one (an unbound unit 0 stays stock)
            const U64 handle = handle_of(params.mTexture.get(), frame);
            if (!handle)
            {
                return;
            }
            sHandles.push_back(handle);
            c.mHandleCount = 1;
            c.mTexMatrix = true;
        }

        // the vertex data: the attributes the program reads (the texture index comes with the positions)
        const U32 needed = sPrograms[slot].mAttributeMask & FS_GDD_ALPHA_ATTRIBS & ~(U32)LLVertexBuffer::MAP_TEXTURE_INDEX;
        U32 fail = 0;
        const FSGDDArenaEntry* entry = nullptr;
        if ((params.mVertexBuffer->getTypeMask() & needed) != needed ||
            !(entry = arena.find(params.mVertexBuffer.get(), frame, &fail)) ||
            (entry->mTypeMask & needed) != needed ||
            params.mOffset + params.mCount > entry->mNumIndices || params.mEnd >= entry->mNumVerts)
        {
            sHandles.resize(c.mFirstHandle);
            c.mHandleCount = 0;
            return;
        }
        c.mEntry = entry;
        c.mSlot = (S32)slot;
    }

    // Phase two: the command of a candidate kept
    void emit(const Candidate& c, const glm::mat4& proj, const glm::mat4& view)
    {
        LLDrawInfo& params = *c.mParams;
        const U32 slot = (U32)c.mSlot;
        const U32 tex_matrix = c.mTexMatrix ? sMatrices.textureIndex(params.mTextureMatrix.get()) : 0;
        U32 data = 0;
        if (slot_material(slot))
        {
            // the uniforms the stock loop sets for a material draw, and its maps
            Material record = {};
            for (U32 i = 0; i < 4; ++i)
            {
                record.mSpecular[i] = params.mSpecColor.mV[i];
            }
            record.mParams[0] = params.mEnvIntensity;
            record.mParams[1] = sMinimumAlpha;     // the blend mode reads none
            record.mParams[2] = params.mFullbright ? 1.f : 0.f;
            for (U32 i = 0; i < 3; ++i)
            {
                const U64 handle = sHandles[c.mFirstHandle + i];
                record.mTex[i * 2] = (U32)(handle & 0xFFFFFFFFull);
                record.mTex[i * 2 + 1] = (U32)(handle >> 32);
            }
            // consecutive draws mostly share their material
            if (sMaterials.empty() || memcmp(&sMaterials.back(), &record, sizeof(Material)) != 0)
            {
                sMaterials.push_back(record);
            }
            data = (U32)sMaterials.size() - 1;
        }
        else
        {
            // the header: the minimum alpha the pass set on the stock program, the texture count; then the handles
            data = (U32)sTextures.size();
            U32 cutoff_bits = 0;
            memcpy(&cutoff_bits, &sMinimumAlpha, sizeof(cutoff_bits));
            sTextures.push_back({ cutoff_bits, c.mHandleCount });
            for (U32 i = 0; i < c.mHandleCount; ++i)
            {
                const U64 handle = sHandles[c.mFirstHandle + i];
                sTextures.push_back({ (U32)(handle & 0xFFFFFFFFull), (U32)(handle >> 32) });
            }
        }

        const U32 command = (U32)sCommands.size();
        sCommands.push_back({ params.mCount, 1, c.mEntry->mIndex + params.mOffset, (GLint)c.mEntry->mVertex, command });
        sRecords.push_back({ sMatrices.modelIndex(params.mModelMatrix, proj, view), tex_matrix, data, 0 });
        sCommandSlot.push_back(slot);
        sCommandParams.push_back(&params);
    }

    // Rigged: every palette the group draws with into the palette buffer, and the offsets into the records
    bool assign_palettes()
    {
        if (!FSSkinPaletteBuffer::place(sCommandParams, sStockRigged, sPaletteOffsets, sPaletteGeneration) ||
            sPaletteOffsets.size() != sRecords.size())
        {
            return false;
        }
        for (size_t c = 0; c < sRecords.size(); ++c)
        {
            sRecords[c].mSkin = sPaletteOffsets[c];
        }
        return true;
    }

    void unbind_uploads()
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        LLVertexBuffer::sGLRenderBuffer = 0;    // or the next setBuffer of the buffer it named skips binding it
    }
}

bool fsGDDAlphaBeginGroup(const std::vector<LLPointer<LLDrawInfo>>& draws, bool& palettes_moved)
{
    LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD);
    LL_RECORD_BLOCK_TIME(FTM_FS_GDD_BUILD_ALPHA);
    FSGDDBuildScope scope(FS_GDD_BUILDER_ALPHA);
    scope.addBatches(draws.size());
    palettes_moved = false;
    sDrawCommand.assign(draws.size(), -1);
    sPendingSlot = -1;
    sPendingCount = 0;
    if (!sPassOn || draws.empty())
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;

    const U32 start_generation = FSSkinPaletteBuffer::generation();
    const U32 frame = LLFrameTimer::getFrameCount();
    FSGDDArena& arena = sPassRigged ? sRiggedArena : sArena;
    arena.beginFrame(frame);
    fsGDDBeginTexturePass();

    // phase one: what each draw would be, in the loop's order (the draws the loop skips have none)
    sCandidates.assign(draws.size(), { nullptr, nullptr, -1, 0, 0, false });
    sHandles.clear();
    for (size_t i = 0; i < draws.size(); ++i)
    {
        LLDrawInfo* params = draws[i].get();
        if (!params || (bool)params->mAvatar != sPassRigged)
        {   // renderAlpha skips it
            continue;
        }
        sCandidates[i].mParams = params;
        // rigged: the stock loop draws nothing for a draw without avatar, skin or palette yet
        if (sPassRigged && !FSSkinPaletteBuffer::drawable(*params))
        {
            continue;
        }
        classify(sCandidates[i], frame, arena);
    }

    // runs: consecutive candidates of one program, a draw the stock loop draws ending them; short ones go stock
    for (size_t i = 0; i < draws.size(); )
    {
        if (sCandidates[i].mSlot < 0)
        {
            ++i;
            continue;
        }
        size_t end = i + 1;
        // the draws the loop skips (another pass's, rigged or not) don't end a run, nothing is drawn for them
        while (end < draws.size() && (sCandidates[end].mSlot == sCandidates[i].mSlot || !sCandidates[end].mParams))
        {
            ++end;
        }
        U32 length = 0;
        for (size_t j = i; j < end; ++j)
        {
            length += sCandidates[j].mSlot >= 0 ? 1 : 0;
        }
        if (length < FS_GDD_ALPHA_MIN_RUN)
        {
            for (size_t j = i; j < end; ++j)
            {
                sCandidates[j].mSlot = -1;
            }
        }
        i = end;
    }

    // phase two: the commands, in order, so a run's are consecutive
    sCommands.clear();
    sRecords.clear();
    sCommandSlot.clear();
    sCommandParams.clear();
    sTextures.clear();
    sMaterials.clear();
    sStockRigged.clear();
    const glm::mat4 proj = FSReverseZ::rasterProjection(gGL.getProjectionMatrix()); // as LLRender::syncMatrices gives the shaders (FSRenderReverseZ)
    const glm::mat4 view = get_current_modelview();
    sMatrices.begin(proj, view, true, true);   // texture matrix 0: identity
    for (size_t i = 0; i < draws.size(); ++i)
    {
        const Candidate& c = sCandidates[i];
        if (!c.mParams)
        {
            continue;
        }
        if (c.mSlot < 0)
        {
            if (sPassRigged)
            {
                sStockRigged.push_back(c.mParams);
            }
            continue;
        }
        sDrawCommand[i] = (S32)sCommands.size();
        emit(c, proj, view);
    }
    if (sCommands.empty())
    {
        return false;
    }

    if (sPassRigged && !assign_palettes())
    {   // no buffer, or it never held still: the group goes the stock way
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenAlpha: the joint palettes could not be put in the palette buffer; "
                                   << "a rigged alpha group is drawn as before" << LL_ENDL;
        sDrawCommand.assign(draws.size(), -1);
        palettes_moved = FSSkinPaletteBuffer::generation() != start_generation;
        return false;
    }
    palettes_moved = FSSkinPaletteBuffer::generation() != start_generation;

    sIndirectBuffer.upload(GL_DRAW_INDIRECT_BUFFER, sCommands.data(), sCommands.size() * sizeof(FSGDDDrawCommand));
    sRecordBuffer.upload(GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
    sMatrices.upload();
    sTextureBuffer.upload(GL_SHADER_STORAGE_BUFFER, sTextures.data(), sTextures.size() * sizeof(FSGDDTexEntry));
    sMaterialBuffer.upload(GL_SHADER_STORAGE_BUFFER, sMaterials.data(), sMaterials.size() * sizeof(Material));
    unbind_uploads();
    return true;
}

S32 fsGDDAlphaCommand(size_t index)
{
    return index < sDrawCommand.size() ? sDrawCommand[index] : -1;
}

void fsGDDAlphaQueue(S32 command, const fs_gdd_alpha_bind_t& bind)
{
    if (command < 0 || (size_t)command >= sCommands.size())
    {
        return;
    }
    const S32 slot = (S32)sCommandSlot[command];
    if (sPendingCount && slot == sPendingSlot && (U32)command == sPendingFirst + sPendingCount)
    {
        ++sPendingCount;
        return;
    }
    fsGDDAlphaFlush(bind);
    sPendingSlot = slot;
    sPendingFirst = (U32)command;
    sPendingCount = 1;
}

void fsGDDAlphaFlush(const fs_gdd_alpha_bind_t& bind)
{
    if (!sPendingCount || sPendingSlot < 0)
    {
        sPendingCount = 0;
        return;
    }
    const U32 slot = (U32)sPendingSlot;
    const U32 first = sPendingFirst;
    const U32 count = sPendingCount;
    sPendingSlot = -1;
    sPendingCount = 0;
    LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("gdd alpha run");

    if (sPassRigged && FSSkinPaletteBuffer::generation() != sPaletteGeneration)
    {   // the palette buffer was replaced since the group's palettes were placed (not expected: they all were
        // before its loop): place them again, and the records with them
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenAlpha: the palette buffer was replaced mid group" << LL_ENDL;
        if (!assign_palettes())
        {
            LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenAlpha: the joint palettes could not be placed again; a run of "
                                       << "rigged alpha draws is left out" << LL_ENDL;
            return;
        }
        sRecordBuffer.upload(GL_ARRAY_BUFFER, sRecords.data(), sRecords.size() * sizeof(Record));
        unbind_uploads();
    }

    LLGLSLShader& shader = sPrograms[slot];
    // as the stock loop sets it for every one of these draws (plain source-over, the pass's alpha factors)
    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA, sAlphaS, sAlphaD);
    bind(shader);
    if (LLGLSLShader::sCurBoundShaderPtr != &shader)
    {
        LL_WARNS_ONCE("Rendering") << "FSPerfGPUDrivenAlpha: " << shader.mName << " failed to bind; a run of alpha draws "
                                   << "is left out" << LL_ENDL;
        return;
    }
    if (slot_family(slot) == F_FULLBRIGHT)
    {   // as the stock loop does when it switches to a fullbright program: the exposure map, if the program reads it
        LLGLSLShader::sFSLazyTextureEnable = true;
        const S32 channel = shader.enableTexture(LLShaderMgr::EXPOSURE_MAP);
        LLGLSLShader::sFSLazyTextureEnable = false;
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->bind(&gPipeline.mExposureMap);
        }
    }
    // What every stock draw syncs before it draws: the light uniforms the forward lighting reads (the program's
    // features ask for them), and the projection the skinned branches read. The model matrices it would set
    // are the records' here.
    gGL.syncMatrices();

    // the inputs; the attributes go into the global vertex array object, never a buffer's own
    LLVertexBuffer::fsUseDefaultVAO();
    sMatrices.bindBase(FS_GDD_ALPHA_MATRIX_BINDING);
    (slot_material(slot) ? sMaterialBuffer : sTextureBuffer).bindBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_ALPHA_DATA_BINDING);
    if (sPassRigged)
    {   // the palette buffer whole; the records hold the offsets
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_ALPHA_SKIN_BINDING, FSSkinPaletteBuffer::glName());
    }
    sIndirectBuffer.bind(GL_DRAW_INDIRECT_BUFFER);
    const GLuint record = (GLuint)sRecordAttrib[slot];
    sRecordBuffer.bind(GL_ARRAY_BUFFER);
    glEnableVertexAttribArray(record);
    glVertexAttribIPointer(record, 4, GL_UNSIGNED_INT, sizeof(Record), sRecordBuffer.at(0));
    glVertexAttribDivisor(record, 1);
    // every attribute the program reads (bind_inputs checked the arenas hold them, classify that the draws have
    // them), texture_index included
    (sPassRigged ? sRiggedArena : sArena).bindForDraw(shader.mAttributeMask);

    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT, sIndirectBuffer.at((size_t)first * sizeof(FSGDDDrawCommand)),
                                (GLsizei)count, 0);

    // undo the inputs: the program's attribute mask doesn't have the record's location, so LLVertexBuffer
    // expects it off and undivided
    glVertexAttribDivisor(record, 0);
    glDisableVertexAttribArray(record);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_ALPHA_MATRIX_BINDING, 0);
    if (sPassRigged)
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_ALPHA_SKIN_BINDING, 0);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, FS_GDD_ALPHA_DATA_BINDING, 0);  // also unbinds the generic binding
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    LLVertexBuffer::unbind();   // the next setBuffer() binds and sets up its own attributes again
    STOP_GLERROR;
}
