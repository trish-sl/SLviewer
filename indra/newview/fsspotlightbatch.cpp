/**
 * @file fsspotlightbatch.cpp
 * @brief Instanced drawing of deferred spotlights that cast no shadow
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#include "fsspotlightbatch.h"

#include "llcamera.h"
#include "lldrawable.h"
#include "llenvironment.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvovolume.h"
#include "m4math.h"
#include "pipeline.h"

#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <numeric>

extern U32 gFrameCount;

FSSpotLightBatch gFSSpotLightBatch;

namespace
{
    constexpr U32 FS_BLOCK_BYTES = FSSpotLightBatch::FS_BLOCK_VEC4 * sizeof(LLVector4); // 16384
    constexpr U32 FS_LIGHT_BYTES = FSSpotLightBatch::FS_VEC4_PER_LIGHT * sizeof(LLVector4); // 160

    // Unit cube faces in the order of fs_face_normal[] in fsBatchedSpotLightV.glsl:
    // -X, +X, -Y, +Y, -Z, +Z, six vertices each (as fspointlightbatch.cpp). The vertex shader
    // finds a vertex's face from gl_VertexID / 6, so winding does not matter and face culling
    // is left off.
    LLPointer<LLVertexBuffer> create_box_vb()
    {
        LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        LLStrider<LLVector3> pos;
        if (!vb->allocateBuffer(36, 0) || !vb->getVertexStrider(pos))
        {
            return nullptr;
        }

        for (U32 axis = 0; axis < 3; ++axis)
        {
            const U32 u = (axis + 1) % 3;
            const U32 v = (axis + 2) % 3;
            for (S32 side = 0; side < 2; ++side)
            {
                const F32 sign = side ? 1.f : -1.f;
                LLVector3 corner[4];
                const F32 uv[4][2] = { { -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } };
                for (U32 i = 0; i < 4; ++i)
                {
                    corner[i].mV[axis] = sign;
                    corner[i].mV[u]    = uv[i][0];
                    corner[i].mV[v]    = uv[i][1];
                }
                *pos++ = corner[0]; *pos++ = corner[1]; *pos++ = corner[2];
                *pos++ = corner[0]; *pos++ = corner[2]; *pos++ = corner[3];
            }
        }

        vb->unmapBuffer();
        return vb;
    }
}

bool FSSpotLightBatch::begin()
{
    static LLCachedControl<bool> batched(gSavedSettings, "FSPerfBatchedSpotLights", false);

    mLights.clear();
    mStaging.clear();

    if (!batched || !gFSBatchedSpotLightProgram.isComplete())
    {
        return false;
    }

    if (mUBOAlignment < 0)
    {
        GLint align = 0;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
        // A draw's range starts at the aligned offset at or before its first light and the shader
        // skips the remainder (fs_spot_base, in vec4s): that needs the remainder to be whole vec4s,
        // true for any power of two, and leaves room for ~100 lights at any alignment seen (<= 256).
        const bool usable = align > 0 && (align & (align - 1)) == 0 && align <= 1024;
        mUBOAlignment = usable ? align : 0;
        LL_INFOS("FSBatchedLights") << "Uniform buffer offset alignment " << align
                                    << (usable ? "" : ", batched spotlights unavailable") << LL_ENDL;
    }

    return mUBOAlignment > 0;
}

void FSSpotLightBatch::add(LLDrawable* drawablep, bool inside, const LLColor3& linear_color, F32 falloff)
{
    LLVOVolume* volume = drawablep ? drawablep->getVOVolume() : nullptr;
    if (!volume)
    {
        return;
    }

    // The projection, as LLPipeline::setupSpotLight computes it; keep the two in step.
    LLVector3 params = volume->getSpotLightParams();

    F32 fov = params.mV[0];
    F32 focus = params.mV[1];

    LLVector3 pos = drawablep->getPositionAgent();
    LLQuaternion quat = volume->getRenderRotation();
    LLVector3 scale = volume->getScale();

    //get near clip plane
    LLVector3 at_axis(0, 0, -scale.mV[2] * 0.5f);
    at_axis *= quat;

    LLVector3 np = pos + at_axis;
    at_axis.normVec();

    //get origin that has given fov for plane np, at_axis, and given scale
    F32 dist = (scale.mV[1] * 0.5f) / tanf(fov * 0.5f);

    LLVector3 origin = np - at_axis * dist;

    //matrix from volume space to agent space
    LLMatrix4 light_mat(quat, LLVector4(origin, 1.f));

    glm::mat4 light_to_agent(glm::make_mat4((F32*)light_mat.mMatrix));
    glm::mat4 light_to_screen = get_current_modelview() * light_to_agent;

    glm::mat4 screen_to_light = glm::inverse(light_to_screen);

    F32 s = volume->getLightRadius() * 1.5f;
    F32 near_clip = dist;
    F32 width = scale.mV[VX];
    F32 height = scale.mV[VY];
    F32 far_clip = s + dist - scale.mV[VZ];

    F32 fovy = fov; // radians
    F32 aspect = width / height;

    glm::mat4 trans(0.5f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.5f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.5f, 0.0f,
                    0.5f, 0.5f, 0.5f, 1.0f);

    glm::vec3 p1(0, 0, -(near_clip + 0.01f));
    glm::vec3 p2(0, 0, -(near_clip + 1.f));

    glm::vec3 screen_origin(0, 0, 0);

    p1 = mul_mat4_vec3(light_to_screen, p1);
    p2 = mul_mat4_vec3(light_to_screen, p2);
    screen_origin = mul_mat4_vec3(light_to_screen, screen_origin);

    glm::vec3 n = p2 - p1;
    n = glm::normalize(n);

    F32 proj_range = far_clip - near_clip;
    glm::mat4 light_proj = glm::perspective(fovy, aspect, near_clip, far_clip);
    screen_to_light = trans * light_proj * screen_to_light;

    LLViewerTexture* img = volume->getLightTexture();
    if (img == NULL)
    {
        img = LLViewerFetchedTexture::sWhiteImagep;
    }
    if (!img)
    {
        return; // no texture at all (shutdown); the stock path would not light it either
    }

    F32 lod_range = logf((F32)img->getWidth()) / logf(2.f);
    F32 ambient_lod = llclamp((proj_range - focus) / proj_range * lod_range, 0.f, 1.f);

    Light light;
    light.mTexture = img;
    light.mFirst = (U32)mStaging.size();
    light.mInside = inside;
    mLights.push_back(light);

    const F32* m = glm::value_ptr(screen_to_light); // column major, as glUniformMatrix4fv takes it
    mStaging.emplace_back(pos.mV[VX], pos.mV[VY], pos.mV[VZ], s);
    mStaging.emplace_back(linear_color.mV[0], linear_color.mV[1], linear_color.mV[2], falloff);
    mStaging.emplace_back(m[0], m[1], m[2], m[3]);
    mStaging.emplace_back(m[4], m[5], m[6], m[7]);
    mStaging.emplace_back(m[8], m[9], m[10], m[11]);
    mStaging.emplace_back(m[12], m[13], m[14], m[15]);
    mStaging.emplace_back(p1.x, p1.y, p1.z, near_clip);
    mStaging.emplace_back(n.x, n.y, n.z, proj_range);
    mStaging.emplace_back(screen_origin.x, screen_origin.y, screen_origin.z, params.mV[2]);
    mStaging.emplace_back(focus, lod_range, ambient_lod, 0.f);
}

// Camera inside the box: every face is drawn with GL_GEQUAL, no depth write, depth clamped.
// A face fragment passes where the scene surface is at or in front of it. Inside the box, each
// pixel's ray leaves the box through exactly one face, so a scene point inside the box passes
// once (at its exit face) and one beyond the box fails: those are exactly the pixels the stock
// full screen pass would not discard either way, since the shader discards everything at or
// beyond the light's radius and that sphere is inscribed in the box. With the camera up to 0.2 m
// outside (the stock rule's margin) the faces it sees from outside are drawn too; they pass only
// where the surface is in front of the box, which the shader discards, so no pixel is lit twice.
// Holes: the exit face can be nearer than the near plane only where every visible surface on the
// ray is beyond it, i.e. outside the box, which lights nothing. It can be beyond the far plane
// for a large light, while the surface is inside the box and before the far plane: clipping would
// drop that pixel, so GL_DEPTH_CLAMP is on for these draws, which turns off near and far
// clipping and clamps the face's depth to the far value, where GEQUAL passes for every surface.
// With FSRenderReverseZ on, LLGLDepthTest flips the function and the far value is 0: the same holds.
void FSSpotLightBatch::flush(LLCamera* camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    const U32 count = (U32)mLights.size();
    U32 groups = 0;
    U32 draws = 0;

    if (count > 0 && camera)
    {
        if (mBoxVB.isNull())
        {
            mBoxVB = create_box_vb();
        }
        if (!mUBO)
        {
            glGenBuffers(1, &mUBO);
        }

        if (mBoxVB.notNull() && mUBO)
        {
            LL_PROFILE_GPU_ZONE("batched spotlights");

            // Draw order: outside lights first, then inside, each sorted by texture. Lights add,
            // so the order changes nothing on screen.
            std::vector<U32> order(count);
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(), [this](U32 a, U32 b)
            {
                const Light& la = mLights[a];
                const Light& lb = mLights[b];
                if (la.mInside != lb.mInside)
                {
                    return !la.mInside;
                }
                return la.mTexture.get() < lb.mTexture.get();
            });

            // One upload, plus a block's worth of padding so the last draw's 16 KB range is backed.
            mData.assign((size_t)count * FS_VEC4_PER_LIGHT + FS_BLOCK_VEC4, LLVector4(0.f, 0.f, 0.f, 0.f));
            for (U32 k = 0; k < count; ++k)
            {
                const U32 src = mLights[order[k]].mFirst;
                std::copy(mStaging.begin() + src, mStaging.begin() + src + FS_VEC4_PER_LIGHT,
                          mData.begin() + (size_t)k * FS_VEC4_PER_LIGHT);
            }

            glBindBuffer(GL_UNIFORM_BUFFER, mUBO);
            glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(mData.size() * sizeof(LLVector4)), mData.data(), GL_STREAM_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            LLGLSLShader& shader = gFSBatchedSpotLightProgram;
            LLGLDisable cull(GL_CULL_FACE);

            gPipeline.bindDeferredShader(shader);

            // Per frame, not per light (the stock loops set classic_mode for every light).
            static LLStaticHashedString s_camera_origin("fs_camera_origin");
            static LLStaticHashedString s_spot_base("fs_spot_base");
            static LLStaticHashedString s_spot_inside("fs_spot_inside");
            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
            shader.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky && psky->canAutoAdjust()) ? 1 : 0);
            shader.uniform3fv(s_camera_origin, 1, camera->getOrigin().mV);

            const S32 channel = shader.enableTexture(LLShaderMgr::DEFERRED_PROJECTION);

            mBoxVB->setBuffer();
            gGL.syncMatrices();

            const U32 align = (U32)mUBOAlignment;

            // Lights [begin, end) of the sorted order, all inside or all outside.
            auto draw_lights = [&](U32 begin, U32 end, bool inside)
            {
                shader.uniform1i(s_spot_inside, inside ? 1 : 0);

                U32 k = begin;
                while (k < end)
                {
                    // One group: the lights sharing this projection texture.
                    LLViewerTexture* tex = mLights[order[k]].mTexture.get();
                    U32 group_end = k + 1;
                    while (group_end < end && mLights[order[group_end]].mTexture.get() == tex)
                    {
                        ++group_end;
                    }
                    if (channel > -1)
                    {
                        gGL.getTexUnit(channel)->bind(tex);
                    }
                    ++groups;

                    while (k < group_end)
                    {
                        const U32 byte_offset = k * FS_LIGHT_BYTES;
                        const U32 range_offset = byte_offset - byte_offset % align;
                        const U32 base = (byte_offset - range_offset) / sizeof(LLVector4);
                        const U32 fit = (FS_BLOCK_VEC4 - base) / FS_VEC4_PER_LIGHT;
                        const U32 n = llmin(group_end - k, fit);

                        glBindBufferRange(GL_UNIFORM_BUFFER, FS_UBO_BINDING, mUBO, (GLintptr)range_offset, FS_BLOCK_BYTES);
                        shader.uniform1i(s_spot_base, (GLint)base);
                        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, n);
                        ++draws;
                        k += n;
                    }
                }
            };

            U32 first_inside = 0;
            while (first_inside < count && !mLights[order[first_inside]].mInside)
            {
                ++first_inside;
            }

            if (first_inside > 0)
            {
                // The stock box path's state: depth tested (LEQUAL), not written.
                LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                draw_lights(0, first_inside, false);
            }
            if (first_inside < count)
            {
                // See above: only surfaces inside the box pass, nothing is clipped away.
                LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_GEQUAL);
                LLGLEnable depth_clamp(GL_DEPTH_CLAMP);
                draw_lights(first_inside, count, true);
            }

            if (channel > -1)
            {
                shader.disableTexture(LLShaderMgr::DEFERRED_PROJECTION);
            }
            gPipeline.unbindDeferredShader(shader);
        }
    }

    mFrameLights = count;
    mFrameGroups = groups;
    mFrameDraws = draws;
    mLights.clear();
    mStaging.clear();

    if (gFrameCount % 300 == 0)
    {
        LL_DEBUGS("FSBatchedLights") << "Batched spotlights: " << mFrameLights << " lights in " << mFrameGroups
                                     << " groups, " << mFrameDraws << " instanced draws (stock path: "
                                     << mFrameLights << " setupSpotLight calls and draws)" << LL_ENDL;
    }
}

// static
void FSSpotLightBatch::setupProgram(LLGLSLShader& shader)
{
    if (!shader.mProgramObject)
    {
        return;
    }
    // With parallel shader compile the program may still be linking: binding it finishes the link
    // (a query on an unlinked program finds no block on AMD, and the binding was never set)
    shader.bind();
    shader.unbind();
    if (!shader.mProgramObject)
    {
        return; // the link failed; its phase is loaded again the classic way
    }
    GLuint index = glGetUniformBlockIndex(shader.mProgramObject, "FSSpotLights");
    if (index != GL_INVALID_INDEX)
    {
        glUniformBlockBinding(shader.mProgramObject, index, FS_UBO_BINDING);
    }
    else
    {
        LL_WARNS("FSBatchedLights") << "FSSpotLights block missing from " << shader.mName << LL_ENDL;
    }
}

void FSSpotLightBatch::release()
{
    mBoxVB = nullptr;
    if (mUBO)
    {
        glDeleteBuffers(1, &mUBO);
        mUBO = 0;
    }
    mLights.clear();
    mLights.shrink_to_fit();
    mStaging.clear();
    mStaging.shrink_to_fit();
    mData.clear();
    mData.shrink_to_fit();
    mUBOAlignment = -1;
}
