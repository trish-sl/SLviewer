/**
 * @file class3/deferred/fsClusteredLightF.glsl
 * @brief Every local point light of a pixel's cluster in one full screen pass (FSRenderClusteredLights)
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

// fsclusteredlights.cpp sorts the point lights into clusters (64 pixel screen tiles by depth
// slices spaced evenly in log depth) and uploads two buffer textures: the lights, and a header
// of (offset, count) per cluster followed by the clusters' lists of light indices. Each pixel
// finds its cluster and lights itself with that list only.
//
// The per light maths is multiPointLightF.glsl's, line for line: the same attenuation, PBR and
// legacy branches, clamps and classic mode scale, so the result is what the stock box and full
// screen passes add up to, within float rounding (the stock passes add in the half float target
// one light at a time, here the sum is taken first).

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform samplerBuffer fs_cl_lights;     // per light: eye space centre + size, linear colour + falloff
uniform usamplerBuffer fs_cl_lists;     // per cluster: offset, count; then the light indices
uniform ivec4 fs_cl_dims;               // tiles across, tiles down, depth slices
uniform vec4 fs_cl_scale;               // tiles per unit of screen coordinate across and down,
                                        // slices per unit of log depth, log of the first slice's depth
uniform int fs_cl_debug;                // 1: add a heat map of the lights per cluster

uniform vec2  screen_res;
uniform mat4  inv_proj;
uniform int classic_mode;

in vec4 vary_fragcoord;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
float blinnPhongLobe(float nh, float glossiness);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);
vec3 clampRadiance(vec3 c);
float fsMicroShadow(float nl, float ao); // deferredUtil.glsl: normal-map micro-shadows, 1 when off
float unpackRoughness(vec2 p);
float calcLegacyDistanceAttenuation(float distance, float falloff);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec2 getScreenCoord(vec4 clip);
vec3 srgb_to_linear(vec3 c);

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

GBufferInfo getGBuffer(vec2 screenpos);
// Avatar skin lighting (fsSkinUtil.glsl)
#ifdef FS_SKIN_LIGHTING
float fsSkinWeightGB(GBufferInfo gb, vec2 tc);
#endif
#ifdef FS_SKIN_DUAL_SPEC
vec3 fsSkinDualLobe(vec3 n, vec3 v, vec3 l, float perceptualRoughness);
vec3 fsSkinLegacySpecular(vec3 n, vec3 v, vec3 l, vec4 spec);
#endif
#ifdef FS_SKIN_TRANS_LOCAL
float fsSkinThickness(vec2 tc, vec3 pos, vec3 n);
vec3 fsSkinLocalTranslucency(vec3 albedo, vec3 n, vec3 l, float thickness_mm, float w, vec3 pos);
vec3 fsSkinAlbedo(GBufferInfo gb);
#endif

#ifdef FS_POINT_SHADOW_MAP
// Shadow maps of the local point lights (FSRenderLocalLightShadows, fsPointShadowMapF.glsl).
// fsclusteredlights.cpp moves the lights that have one to the front of the light buffer: light
// idx < fs_lls_count samples slot fs_lls_index[idx].
uniform int fs_lls_count;
uniform int fs_lls_index[FS_LLS_MAX];

vec3 fsPointShadowMap(int slot, vec3 pos, vec3 n, vec3 shading_light);

// The shadow on a lit pixel from light idx: its shadow map where it has one
vec3 fsLocalShadow(int idx, vec3 pos, vec3 n, vec3 shading_light)
{
    if (idx < fs_lls_count)
    {
        return fsPointShadowMap(fs_lls_index[idx], pos, n, shading_light);
    }
    return vec3(1.0);
}
#endif

// The cluster of a pixel, as fsclusteredlights.cpp bins them
int clusterOf(vec2 tc, float depth)
{
    ivec2 tile = clamp(ivec2(tc * fs_cl_scale.xy), ivec2(0), fs_cl_dims.xy - ivec2(1));
    // nearer than 1 m (where the slices start) is slice 0, past the farthest light the last slice
    float s = floor((log(max(depth, 1e-6)) - fs_cl_scale.w) * fs_cl_scale.z);
    int slice = int(clamp(s, 0.0, float(fs_cl_dims.z - 1)));
    return (slice * fs_cl_dims.y + tile.y) * fs_cl_dims.x + tile.x;
}

vec3 heatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(t * 3.0 - 1.0, 1.5 - abs(t * 3.0 - 1.5), 1.0 - t * 3.0), vec3(0.0), vec3(1.0));
}

void main()
{
    vec3 final_color = vec3(0, 0, 0);
    vec2 tc          = getScreenCoord(vary_fragcoord);
    // The sky has no surface to light. It fell in the last depth slice, so it walked that
    // slice's lights only to find every one out of range (or a far plane at infinity). The
    // debug heat map still covers it.
    float depth      = getDepth(tc);
    if (isFarDepth(depth) && fs_cl_debug == 0)
    {
        discard;
    }
    vec3 pos         = getPositionWithDepth(tc, depth).xyz;

    int cluster = clusterOf(tc, -pos.z);
    int first = int(texelFetch(fs_cl_lists, cluster * 2).r);
    int count = int(texelFetch(fs_cl_lists, cluster * 2 + 1).r);
    // a cluster lists each light at most once: a broken list can't loop for long (a GPU timeout)
    count = min(count, textureSize(fs_cl_lights) / 2);

    if (count <= 0)
    {
        discard;
    }

    // Debug view: blue for one light in the cluster, through green to red at 32 or more
    vec3 heat = fs_cl_debug != 0 ? heatColor(float(count) / 32.0) * 0.25 : vec3(0.0);

    GBufferInfo gb = getGBuffer(tc);
#ifdef FS_SKIN_LIGHTING
    float fs_skin = fsSkinWeightGB(gb, tc); // how much of this pixel is avatar skin, 0 elsewhere
#endif
#ifdef FS_SKIN_TRANS_LOCAL
    // Skin: how thick it is where lights may shine through it from behind, once for every light
    float fs_thick = fs_skin > 0.0 ? fsSkinThickness(tc, pos, gb.normal) : 0.0;
    vec3 fs_albedo = fsSkinAlbedo(gb);
#endif

    vec3 n = gb.normal;

    vec4 spec    = gb.specular;
    vec3 diffuse = gb.albedo.rgb;

    vec3  h, l, v = -normalize(pos);
    float nh, nv, vh, lightDist;

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = unpackRoughness(spec.ga);
        float metallic = orm.b;
        vec3 baseColor = diffuse.rgb;

        vec3 diffuseColor;
        vec3 specularColor;
        calcDiffuseSpecular(baseColor, metallic, diffuseColor, specularColor);

        for (int i = 0; i < count; ++i)
        {
            int idx = int(texelFetch(fs_cl_lists, first + i).r);
            vec4 light = texelFetch(fs_cl_lights, idx * 2);
            vec4 light_col = texelFetch(fs_cl_lights, idx * 2 + 1);

            vec3  lightColor = light_col.rgb; // linear, see pipeline.cpp: volume->getLightLinearColor()
            float falloff    = light_col.a;
            float lightSize  = light.w;
            vec3  lv         = light.xyz - pos;

            lightDist = length(lv);

            float dist = lightDist / lightSize;
            // A back-facing light gives NdotL = 0 inside pbrPunctual, so its contribution is
            // exactly zero -- skip the BRDF rather than evaluate it and multiply by nothing.
            if (dist <= 1.0 && dot(n, lv) > 0.0)
            {
                lv /= max(lightDist, 1e-8);

                float dist_atten = calcLegacyDistanceAttenuation(dist, falloff);

                vec3 intensity = dist_atten * lightColor * PUNCTUAL_LIGHT_SCALE;
                float nl = 0;
                vec3 diff = vec3(0);
                vec3 specPunc = vec3(0);
                pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, lv, nl, diff, specPunc);
#ifdef FS_SKIN_DUAL_SPEC
                if (fs_skin > 0.0)
                {
                    specPunc = mix(specPunc, fsSkinDualLobe(n, v, lv, perceptualRoughness), fs_skin); // skin: two lobes
                }
#endif
                intensity *= fsMicroShadow(nl, orm.r);
#ifdef FS_POINT_SHADOW_MAP
                intensity *= fsLocalShadow(idx, pos, n, light.xyz);
#endif
                final_color += intensity * clampRadiance(nl * (diff + specPunc));
            }
#ifdef FS_SKIN_TRANS_LOCAL
            else if (fs_thick > 0.0 && dist <= 1.0) // skin: light through it from behind
            {
                final_color += fsSkinLocalTranslucency(fs_albedo, n, lv / max(lightDist, 1e-8), fs_thick, fs_skin, pos) *
                               lightColor * calcLegacyDistanceAttenuation(dist, falloff);
            }
#endif
        }
    }
    else
    {
        diffuse = srgb_to_linear(diffuse);
        spec.rgb = srgb_to_linear(spec.rgb);

        for (int i = 0; i < count; ++i)
        {
            int idx = int(texelFetch(fs_cl_lists, first + i).r);
            vec4 light = texelFetch(fs_cl_lights, idx * 2);
            vec4 light_col = texelFetch(fs_cl_lights, idx * 2 + 1);

            vec3  lv   = light.xyz - pos;
            float dist = length(lv);
            dist /= light.w;
            if (dist <= 1.0)
            {
                float nl = dot(n, lv);
                if (nl > 0.0)
                {
                    float lightDist;
                    calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

                    float fa         = light_col.a;
                    float dist_atten = calcLegacyDistanceAttenuation(dist, fa);

                    float lit = nl * dist_atten;

                    vec3 col = light_col.rgb * lit * diffuse;

                    if (spec.a > 0.0)
                    {
                        lit        = min(nl * 6.0, 1.0) * dist_atten;
                        float f = 1 - vh;
                        float f2 = f * f;
                        float fres = (f2 * f2 * f) * 0.4 + 0.5;

                        float gtdenom = 2 * nh;
                        float gt      = max(0, min(gtdenom * nv / vh, gtdenom * nl / vh));

                        if (nh > 0.0)
                        {
                            float scol = fres * blinnPhongLobe(nh, spec.a) * gt / (nh * max(nl, 1e-6));
#ifdef FS_SKIN_DUAL_SPEC
                            col += lit * scol * light_col.rgb * spec.rgb * (1.0 - fs_skin); // skin takes the two-lobe specular instead
#else
                            col += lit * scol * light_col.rgb * spec.rgb;
#endif
                        }
                    }
#ifdef FS_SKIN_DUAL_SPEC
                    if (fs_skin > 0.0) // skin: the two-lobe specular, also where legacy has none
                    {
                        col += fsSkinLegacySpecular(n, v, l, spec) * light_col.rgb * (dist_atten * fs_skin);
                    }
#endif

                    // Bounded per light, as the stock passes do
#ifdef FS_POINT_SHADOW_MAP
                    final_color += clampRadiance(col) * fsLocalShadow(idx, pos, n, light.xyz);
#else
                    final_color += clampRadiance(col);
#endif
                }
#ifdef FS_SKIN_TRANS_LOCAL
                else if (fs_thick > 0.0) // skin: light through it from behind
                {
                    final_color += fsSkinLocalTranslucency(fs_albedo, n, normalize(lv), fs_thick, fs_skin, pos) *
                                   light_col.rgb * calcLegacyDistanceAttenuation(dist, light_col.a);
                }
#endif
            }
        }
    }
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    frag_color.rgb = max(final_color * final_scale, vec3(0)) + heat;
    frag_color.a   = 0.0;
}
