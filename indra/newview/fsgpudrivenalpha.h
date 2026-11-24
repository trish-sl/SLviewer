/**
 * @file fsgpudrivenalpha.h
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

#ifndef FS_GPUDRIVENALPHA_H
#define FS_GPUDRIVENALPHA_H

#include "stdtypes.h"
#include "llpointer.h"
#include "llrender.h"

#include <functional>
#include <vector>

class LLDrawInfo;
class LLGLSLShader;

// gDeferredAlphaProgram and gDeferredFullbrightAlphaMaskAlphaProgram built with FS_GDD_LEGACY, the blended
// gDeferredMaterialProgram permutations built with FS_GDD_MATERIAL, and (FSPerfGPUDrivenRigged) their skinned
// variants. Built only while FSPerfGPUDrivenAlpha is on, the driver has gGLManager.mFSHasGPUDrivenDraws and
// bindless textures work (NVIDIA, fsGDDBindlessSetup); the skinned ones only when fsGDDRiggedLoadable
// (fsgpudrivenmaterials.h) holds as well. After the stock programs are set up (via fsGDDLoadShaders).
void fsGDDAlphaLoadShaders();
void fsGDDAlphaUnloadShaders();
// Frees the arenas and the per draw buffers.
void fsGDDAlphaReleaseGL();

// The programs built, for LLDrawPoolAlpha::renderPostDeferred to prepare as it prepares the stock ones
// (prepare_alpha_shader).
void fsGDDAlphaPrograms(std::vector<LLGLSLShader*>& programs);
// Marks their environment uniforms dirty, with the stock programs' (LLEnvironment::update).
void fsGDDAlphaMarkUniformsDirty();

// The bind of a program, as LLDrawPoolAlpha::renderAlpha binds its stock programs (fs_bind_program there:
// bindDeferredShaderFast, the pass's uniforms once)
typedef std::function<void(LLGLSLShader&)> fs_gdd_alpha_bind_t;

// LLDrawPoolAlpha::renderAlpha, before its group loop. False when the pass stays stock: the feature off, or
// a pass it doesn't take (the caller checks HUDs, impostors, probes, depth only, WBOIT and hair passes);
// simple and fullbright are the pass's stock programs, which must be gDeferredAlphaProgram and
// gDeferredFullbrightAlphaMaskAlphaProgram. alpha_s/alpha_d: the pass's alpha blend factors, minimum_alpha
// what the pass sets on its programs.
// no_discard: FSRenderAlphaRiggedNoDiscard, the rigged pass may draw the draws that could never fail the alpha test
// with the program built without it (FSAvatarAlpha::noDiscardDraw), as the stock loop does.
bool fsGDDAlphaBeginPass(bool rigged, LLGLSLShader* simple, LLGLSLShader* fullbright, LLRender::eBlendFactor alpha_s,
                         LLRender::eBlendFactor alpha_d, F32 minimum_alpha, bool no_discard);
// Before a group's draw loop: works out which of its draws go into multi-draws, and uploads them. True if
// any does. palettes_moved: the palette buffer got new storage meanwhile, so a palette range the stock loop
// remembers as bound (its last avatar and mesh) is gone.
bool fsGDDAlphaBeginGroup(const std::vector<LLPointer<LLDrawInfo>>& draws, bool& palettes_moved);
// The command of the group's draw at index, or -1 when the stock loop draws it.
S32 fsGDDAlphaCommand(size_t index);
// In the group's draw loop, in place of the stock draw: adds the command to the pending run, drawing that
// first if this one can't extend it.
void fsGDDAlphaQueue(S32 command, const fs_gdd_alpha_bind_t& bind);
// Draws the pending run (before a stock draw, and at the end of the group's loop). Leaves its program bound.
void fsGDDAlphaFlush(const fs_gdd_alpha_bind_t& bind);
void fsGDDAlphaEndPass();

#endif // FS_GPUDRIVENALPHA_H
