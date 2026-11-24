/**
 * @file fsavatarcostlog.cpp
 * @brief Logs what each nearby avatar puts into the frame (FSRenderGPUTimings)
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

#include "fsavatarcostlog.h"

#include "llagentcamera.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llface.h"
#include "llmaterial.h"
#include "lltextureentry.h"
#include "llviewertexture.h"
#include "llvoavatar.h"

#include <algorithm>
#include <vector>

namespace
{
    // Why an attachment face is in the alpha blend pass. The first three could be drawn as cutout
    // (alpha mask) in the deferred pass instead: their texture has no soft alpha, only the stock
    // viewer never auto-masks rigged faces (LLFace::canRenderAsMask).
    enum AlphaClass
    {
        AC_OPAQUE_TEXTURE,  // the texture has no alpha channel, or none of its alpha is below full
        AC_MASK_TEXTURE,    // the texture's alpha is (close to) on/off, no blend material
        AC_MASK_BLEND_MAT,  // same texture, but a material asks for alpha blending
        AC_SOFT_TEXTURE,    // the texture has real partial alpha: has to blend
        AC_TINT,            // the face colour's alpha is between 0 and 1: has to blend
        AC_GLOW,            // glowing: the mask path doesn't do glow
        AC_PBR,             // a PBR material in blend mode
        AC_NO_TEXTURE,      // texture not loaded yet, can't tell
        AC_COUNT
    };

    const char* const ALPHA_CLASS_NAMES[AC_COUNT] =
    {
        "opaque texture", "on/off alpha texture", "on/off alpha texture with a blend material", "soft alpha texture",
        "face colour alpha", "glow", "PBR blend", "texture not loaded"
    };

    // The texture's alpha, as far as it is known
    enum TextureAlpha
    {
        TXA_OPAQUE,      // no alpha channel, or none of its alpha is below full
        TXA_MASK,        // on/off
        TXA_SOFT,        // real partial alpha
        TXA_UNKNOWN      // not loaded yet
    };

    TextureAlpha texture_alpha(const LLFace* face)
    {
        LLViewerTexture* tex = face->getTexture();
        if (!tex || !tex->hasGLTexture() || tex->getDiscardLevel() < 0)
        {
            return TXA_UNKNOWN;
        }
        if (tex->getComponents() != 4 && tex->getComponents() != 2)
        {
            return TXA_OPAQUE;
        }
        return tex->getIsAlphaMask() ? TXA_MASK : TXA_SOFT;
    }

    // The texture is looked at before the tint: a tinted face whose texture has soft alpha blends for the
    // texture's sake whatever its tint, so the tint classes hold only the faces the tint alone sends to blend
    AlphaClass classify(const LLFace* face)
    {
        const LLTextureEntry* te = face->getTextureEntry();
        if (te && te->getGLTFRenderMaterial())
        {
            return AC_PBR;
        }
        const TextureAlpha ta = texture_alpha(face);
        if (ta == TXA_SOFT)
        {
            return AC_SOFT_TEXTURE;
        }
        if (te && te->getColor().mV[3] < 1.f)
        {
            return AC_TINT;
        }
        if (te && te->getGlow() > 0.f)
        {
            return AC_GLOW;
        }
        if (ta == TXA_UNKNOWN)
        {
            return AC_NO_TEXTURE;
        }
        if (ta == TXA_OPAQUE)
        {
            return AC_OPAQUE_TEXTURE;
        }
        LLMaterial* mat = te ? te->getMaterialParams().get() : nullptr;
        if (mat && mat->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND)
        {
            return AC_MASK_BLEND_MAT;
        }
        return AC_MASK_TEXTURE;
    }

    // Histogram of the face colour alpha of the tinted rigged alpha faces (any texture): which threshold of
    // FSRenderAvatarTintOpaqueAlpha would take how much out of the blend pass
    enum TintBucket
    {
        TB_NEAR_OPAQUE,     // [0.95, 0.999)
        TB_HALF,            // [0.5, 0.95)
        TB_SHEER,           // (0, 0.5)
        TB_COUNT
    };
    const char* const TINT_BUCKET_NAMES[TB_COUNT] = { "0.95 to 0.999", "0.5 to 0.95", "under 0.5" };
    // split by the texture: opaque, or with an alpha channel (on/off or soft) or not loaded yet
    const char* const TINT_TEXTURE_NAMES[2] = { "opaque texture", "alpha texture or not loaded" };

    S32 tint_bucket(F32 alpha)
    {
        return alpha >= 0.95f ? TB_NEAR_OPAQUE : alpha >= 0.5f ? TB_HALF : TB_SHEER;
    }

    struct Row
    {
        LLVOAvatar* mAvatar = nullptr;
        F32 mDistance = 0.f;
        S32 mPrims = 0;
        S32 mFaces = 0;
        S32 mAlphaFaces = 0;
        U32 mTris = 0;
        U32 mAlphaTris = 0;
        S32 mClassFaces[AC_COUNT] = {};    // rigged alpha faces only
        U32 mClassTris[AC_COUNT] = {};
        S32 mUnriggedAlphaFaces = 0;
        U32 mUnriggedAlphaTris = 0;
        S32 mHiddenFaces = 0;               // face colour alpha 0: not drawn, counted apart
        U32 mHiddenTris = 0;
        S32 mTintFaces[TB_COUNT][2] = {};   // rigged alpha faces with a face colour alpha below 1, by bucket and texture
        U32 mTintTris[TB_COUNT][2] = {};

        U32 maskableTris() const
        {
            return mClassTris[AC_OPAQUE_TEXTURE] + mClassTris[AC_MASK_TEXTURE] + mClassTris[AC_MASK_BLEND_MAT];
        }
    };

    // The object, then its children: an avatar's children are its attachments
    void count(LLViewerObject* obj, Row& row, bool is_root)
    {
        if (!obj || obj->isDead() || (!is_root && obj->isHUDAttachment()))
        {
            return; // HUDs are drawn with the UI, not in the world passes
        }
        if (!is_root)
        {
            ++row.mPrims;
        }
        if (LLDrawable* drawable = obj->mDrawable)
        {
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                LLFace* face = drawable->getFace(i);
                if (!face || !face->hasGeometry())
                {
                    continue;
                }
                const U32 tris = face->getIndicesCount() / 3;
                const LLTextureEntry* te = face->getTextureEntry();
                if (face->getPoolType() == LLDrawPool::POOL_ALPHA && te && !te->getGLTFRenderMaterial() &&
                    face->getFaceColor().mV[3] <= 0.f)
                {   // fully transparent (how mesh bodies hide parts): not drawn (PASS_ALPHA_INVISIBLE)
                    ++row.mHiddenFaces;
                    row.mHiddenTris += tris;
                    continue;
                }
                ++row.mFaces;
                row.mTris += tris;
                if (face->getPoolType() == LLDrawPool::POOL_ALPHA)
                {
                    ++row.mAlphaFaces;
                    row.mAlphaTris += tris;
                    if (face->isState(LLFace::RIGGED))
                    {
                        const AlphaClass c = classify(face);
                        ++row.mClassFaces[c];
                        row.mClassTris[c] += tris;
                        const F32 tint = te ? te->getColor().mV[3] : 1.f;
                        if (c != AC_PBR && tint < 1.f)
                        {
                            const S32 opaque_tex = texture_alpha(face) == TXA_OPAQUE ? 0 : 1;
                            ++row.mTintFaces[tint_bucket(tint)][opaque_tex];
                            row.mTintTris[tint_bucket(tint)][opaque_tex] += tris;
                        }
                    }
                    else
                    {
                        ++row.mUnriggedAlphaFaces;
                        row.mUnriggedAlphaTris += tris;
                    }
                }
            }
        }
        for (const auto& child : obj->getChildren())
        {
            count(child, row, false);
        }
    }

    const char* state_of(LLVOAvatar* avatar)
    {
        if (avatar->isVisuallyMuted())
        {
            return "muted or too complex";
        }
        if (avatar->isImpostor())
        {
            return "impostor";
        }
        if (!avatar->isVisible())
        {
            return "out of view";
        }
        return "drawn";
    }

    void log_costs()
    {
        const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
        std::vector<Row> rows;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar())
            {
                continue; // animesh is counted under the avatar wearing it
            }
            Row row;
            row.mAvatar = avatar;
            row.mDistance = (F32)dist_vec(avatar->getPositionGlobal(), camera);
            count(avatar, row, true);
            rows.push_back(row);
        }
        if (rows.empty())
        {
            return;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
            {
                return a.mAlphaFaces != b.mAlphaFaces ? a.mAlphaFaces > b.mAlphaFaces : a.mTris > b.mTris;
            });

        S32 drawn = 0;
        S32 drawn_alpha_faces = 0;
        U32 drawn_tris = 0;
        U32 drawn_alpha_tris = 0;
        Row drawn_sum; // alpha classes summed over the avatars drawn in full
        for (const Row& row : rows)
        {
            if (!strcmp(state_of(row.mAvatar), "drawn"))
            {
                ++drawn;
                drawn_alpha_faces += row.mAlphaFaces;
                drawn_tris += row.mTris;
                drawn_alpha_tris += row.mAlphaTris;
                for (S32 c = 0; c < AC_COUNT; ++c)
                {
                    drawn_sum.mClassFaces[c] += row.mClassFaces[c];
                    drawn_sum.mClassTris[c] += row.mClassTris[c];
                }
                drawn_sum.mUnriggedAlphaFaces += row.mUnriggedAlphaFaces;
                drawn_sum.mUnriggedAlphaTris += row.mUnriggedAlphaTris;
                drawn_sum.mHiddenFaces += row.mHiddenFaces;
                drawn_sum.mHiddenTris += row.mHiddenTris;
                for (S32 b = 0; b < TB_COUNT; ++b)
                {
                    for (S32 t = 0; t < 2; ++t)
                    {
                        drawn_sum.mTintFaces[b][t] += row.mTintFaces[b][t];
                        drawn_sum.mTintTris[b][t] += row.mTintTris[b][t];
                    }
                }
            }
        }

        std::string line;
        const size_t shown = llmin(rows.size(), (size_t)8);
        for (size_t i = 0; i < shown; ++i)
        {
            const Row& row = rows[i];
            LLVOAvatar* avatar = row.mAvatar;
            line += llformat("\n    %s%s: %s, %.0f m, complexity %u, %d attachments, %d prims, faces %d (alpha %d), triangles %uk (alpha %uk, could be cutout %uk)",
                             avatar->getFullname().c_str(), avatar->isSelf() ? " (you)" : "", state_of(avatar), row.mDistance,
                             avatar->getVisualComplexity(), avatar->getAttachmentCount(), row.mPrims, row.mFaces, row.mAlphaFaces,
                             (row.mTris + 500) / 1000, (row.mAlphaTris + 500) / 1000, (row.maskableTris() + 500) / 1000);
        }
        LL_INFOS("FSAvatarCost") << "Avatars " << rows.size() << ", " << drawn << " drawn in full; those have " << drawn_alpha_faces
                                 << " alpha faces and " << (drawn_tris + 500) / 1000 << "k triangles; most alpha faces first:" << line << LL_ENDL;

        if (drawn > 0 && drawn_alpha_faces > 0)
        {
            // Measures whether drawing on/off alpha rigged faces as cutout is worth doing: those
            // would leave the forward alpha pass for the deferred one
            std::string classes;
            for (S32 c = 0; c < AC_COUNT; ++c)
            {
                if (drawn_sum.mClassFaces[c] > 0)
                {
                    classes += llformat("%s%s %d faces %uk triangles", classes.empty() ? "" : "; ", ALPHA_CLASS_NAMES[c],
                                        drawn_sum.mClassFaces[c], (drawn_sum.mClassTris[c] + 500) / 1000);
                }
            }
            const U32 maskable = drawn_sum.maskableTris();
            LL_INFOS("FSAvatarCost") << "Alpha blended faces of the avatars drawn in full, why they blend: rigged: " << classes
                                     << "; not rigged (already auto-masked where they could be): " << drawn_sum.mUnriggedAlphaFaces
                                     << " faces " << (drawn_sum.mUnriggedAlphaTris + 500) / 1000 << "k triangles. Could be cutout: "
                                     << (maskable + 500) / 1000 << "k of " << (drawn_alpha_tris + 500) / 1000 << "k alpha triangles ("
                                     << (drawn_alpha_tris ? maskable * 100 / drawn_alpha_tris : 0)
                                     << "%). Hidden (face colour alpha 0, not drawn, left out above): " << drawn_sum.mHiddenFaces
                                     << " faces " << (drawn_sum.mHiddenTris + 500) / 1000 << "k triangles" << LL_ENDL;

            // Which FSRenderAvatarTintOpaqueAlpha threshold would take how much out of the blend pass: the
            // tinted rigged faces by their face colour alpha, those with an opaque texture apart (only those can
            // be drawn opaque in the deferred pass; a texture with alpha has to blend whatever the tint)
            std::string tints;
            for (S32 b = 0; b < TB_COUNT; ++b)
            {
                for (S32 t = 0; t < 2; ++t)
                {
                    if (drawn_sum.mTintFaces[b][t] > 0)
                    {
                        tints += llformat("%s%s, %s: %d faces %uk triangles", tints.empty() ? "" : "; ", TINT_BUCKET_NAMES[b],
                                          TINT_TEXTURE_NAMES[t], drawn_sum.mTintFaces[b][t], (drawn_sum.mTintTris[b][t] + 500) / 1000);
                    }
                }
            }
            if (!tints.empty())
            {
                LL_INFOS("FSAvatarCost") << "Tinted rigged alpha faces of the avatars drawn in full, by face colour alpha: " << tints
                                         << LL_ENDL;
            }
        }
    }
}

void FSAvatarCostLog::frame(bool enabled, float interval_seconds)
{
    static LLTimer timer;
    if (!enabled)
    {
        timer.reset();
        return;
    }
    if (timer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
    {
        timer.reset();
        log_costs();
    }
}
