/**
 * @file screenSpaceReflTraceF.glsl
 * @brief The opaque surfaces' screen-space reflections, traced into their own buffer before lighting
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

// The opaque surfaces' screen-space reflection, traced before the lighting pass into the SSR
// buffer (fsssrbuffer.cpp), at RenderScreenSpaceReflectionResolutionMultiplier times the screen
// size, after the graphics care package's trace pass. Texel t traces the full-resolution pixel at
// its centre (fsSSRTracedPixel) with exactly what softenLightF would hand the march there (same
// glossiness, same direction, the wet shoreline included). The resolve blends it with the last
// frames (screenSpaceReflResolveF.glsl) and softenLightF's upsample reads it back weighted by how
// well the traced pixel's depth and normal match its own.
//
// Output: the reflection colour compressed (ssrCompress) and premultiplied by its blend weight,
// and the weight; 0 where nothing was traced (sky, rough surfaces). Premultiplied, so the temporal
// blend and the blur mips can average it directly.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform vec2 fs_ssr_full_res;   // full-resolution size in pixels
uniform vec2 fs_ssr_res;        // this target's size in pixels
uniform sampler2D sceneMap;
uniform vec4 waterPlane;

float getDepth(vec2 pos_screen);
vec4 getNorm(vec2 screenpos);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
bool isFarDepth(float d);
GBufferInfo getGBuffer(vec2 screenpos);
float unpackRoughness(vec2 p);
vec3 getSpecularDominantDir(vec3 n, vec3 r, float perceptualRoughness);
float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, vec3 rayDir, inout vec4 collectedColor, sampler2D source, float glossiness);
float ssrMinGlossiness();
vec3 ssrCompress(vec3 c);
#ifdef FS_WET_SHORE
float fsWetShore(vec3 pos, vec3 norm, vec4 water_plane);
#endif

// Specular anti-aliasing for the reflection (after Tokuyoshi and Kaplanyan): the roughness a
// surface has for the trace, widened by how much its shading normal changes from one pixel to the
// next. Fur, carpet and other busy normal maps, and the curved edge of an object, change it
// within a pixel; traced as the smooth surface their roughness says they are, every pixel's ray
// went somewhere else and the reflection sparkled, worst near silhouettes where the Fresnel term
// is strong. Widened, they read the blurred cone and fade to the probes as the roughness reaches
// RenderScreenSpaceReflectionMaxRoughness, smoothly from pixel to pixel.
float fsSSRFilteredRoughness(ivec2 full, vec3 n, float roughness)
{
    ivec2 last = ivec2(fs_ssr_full_res) - 1;
    vec3 l = getNorm((vec2(clamp(full - ivec2(1, 0), ivec2(0), last)) + 0.5) / fs_ssr_full_res).xyz;
    vec3 r = getNorm((vec2(clamp(full + ivec2(1, 0), ivec2(0), last)) + 0.5) / fs_ssr_full_res).xyz;
    vec3 b = getNorm((vec2(clamp(full - ivec2(0, 1), ivec2(0), last)) + 0.5) / fs_ssr_full_res).xyz;
    vec3 t = getNorm((vec2(clamp(full + ivec2(0, 1), ivec2(0), last)) + 0.5) / fs_ssr_full_res).xyz;
    // the larger step on each axis, so a silhouette on either side counts
    vec3 dx = (dot(r - n, r - n) > dot(n - l, n - l)) ? r - n : n - l;
    vec3 dy = (dot(t - n, t - n) > dot(n - b, n - b)) ? t - n : n - b;
    float variance = 0.25 * (dot(dx, dx) + dot(dy, dy));
    float kernel = min(2.0 * variance, 0.3);
    float a = roughness * roughness;
    return sqrt(sqrt(clamp(a * a + kernel, 0.0, 1.0)));
}

void main()
{
    // the full-resolution pixel under this texel's centre (fsSSRTracedPixel in softenLightF)
    ivec2 full = min(ivec2(gl_FragCoord.xy * (fs_ssr_full_res / fs_ssr_res)), ivec2(fs_ssr_full_res) - 1);
    vec2 tc = (vec2(full) + 0.5) / fs_ssr_full_res;

    float depth = getDepth(tc);
    if (isFarDepth(depth))
    {
        frag_color = vec4(0);
        return;
    }

    GBufferInfo gb = getGBuffer(tc);
    bool is_pbr = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR);
    if (!is_pbr && (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS)))
    {
        frag_color = vec4(0); // sky: softenLightF copies a colour there and reflects nothing
        return;
    }

    vec4 pos = getPositionWithDepth(tc, depth);

    float wet = 0.0;
#ifdef FS_WET_SHORE
    wet = fsWetShore(pos.xyz, gb.normal, waterPlane);
#endif

    vec3 refl = normalize(reflect(pos.xyz, gb.normal));
    vec3 dir;
    float gloss;
    if (is_pbr)
    {
        float roughness = unpackRoughness(gb.specular.ga);
        roughness = mix(roughness, min(roughness, 0.15), wet);
        gloss = 1.0 - roughness;
        dir = getSpecularDominantDir(gb.normal, refl, roughness);
    }
    else
    {
        gloss = mix(gb.specular.a, max(gb.specular.a, 0.8), wet);
        dir = refl;
    }
    // the direction stays the surface's own; only the trace's roughness (its cone and its fade) widens
    gloss = 1.0 - fsSSRFilteredRoughness(full, gb.normal, 1.0 - gloss);

    vec4 ssr = vec4(0);
    if (gloss > ssrMinGlossiness())
    {
        tapScreenSpaceReflection(1, tc, pos.xyz, gb.normal, dir, ssr, sceneMap, gloss);
    }
    ssr.a = clamp(ssr.a, 0.0, 1.0);
    frag_color = vec4(ssrCompress(ssr.rgb) * ssr.a, ssr.a);
}
