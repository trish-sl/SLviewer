/**
 * @file alphaF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

//class2/deferred/alphaF.glsl

/*[EXTRA_CODE_HERE]*/

#ifdef FS_GDD_LEGACY
#extension GL_ARB_bindless_texture : require
// <FS:Trish> FSPerfGPUDrivenAlpha (alphaV.glsl): the texture the stock diffuseLookup would pick, as a
// bindless handle. minimum_alpha stays the uniform, which the pass sets as it does on the stock program.
flat in uvec2 fs_gdd_diffuse;
vec4 diffuseLookup(vec2 texcoord)
{
    return texture(sampler2D(fs_gdd_diffuse), texcoord);
}
// </FS:Trish>
#endif

#define INDEXED 1
#define NON_INDEXED 2
#define NON_INDEXED_NO_COLOR 3

#ifdef FS_WBOIT
// Weighted blended order-independent transparency (FSRenderAlphaWBOIT, see fsWBOITCompositeF).
// Same block in class2 alphaF, class1 fullbrightF, class3 materialF and class2 pbralphaF.
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 fs_wboit_weight;
uniform int fs_wboit; // 1 while accumulating, 0 for the stock sorted output
#else
out vec4 frag_color;
#endif

#ifdef FS_HAIR
// Two-pass rigged alpha (FSRenderHairTwoPass, see LLDrawPoolAlpha::renderRiggedAlphaTwoPass).
// Same block in class2 alphaF, class1 fullbrightF, class3 materialF and class2 pbralphaF.
// fs_hair_pass: 0 stock, 1 solid core only (coverage >= fs_hair_cut, writes depth),
// 2 soft fringe only (coverage < fs_hair_cut, depth tested only), 3 depth only.
uniform int fs_hair_pass;
uniform float fs_hair_cut;
#endif

uniform mat3 env_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int classic_mode;

#ifdef USE_DIFFUSE_TEX
uniform sampler2D diffuseMap;
#endif

in vec3 vary_fragcoord;
in vec3 vary_position;
in vec2 vary_texcoord0;
in vec3 vary_norm;

#ifdef USE_VERTEX_COLOR
in vec4 vertex_color; //vertex color should be treated as sRGB
#endif

uniform float minimum_alpha;

uniform mat4 proj_mat;
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;
uniform vec4 light_position[8];
uniform vec3 light_direction[8];
uniform vec4 light_attenuation[8];
uniform vec3 light_diffuse[8];
uniform vec2 light_deferred_attenuation[8];

void waterClip(vec3 pos);

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
// <FS:Trish> The out parameters named in the definition's order (atmosphericsFuncs.glsl): the calls
// pass them by position, and the swapped names would mislead the next caller
//void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 additive, out vec3 atten);
// </FS:Trish>

#ifdef HAS_SUN_SHADOW
float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen);
#endif

float getAmbientClamp();
vec3 fsScrubNaN(vec3 c); // <FS:Trish/> deferredUtil.glsl

void mirrorClip(vec3 pos);

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);

// The shared legacy punctual model, not a copy of it -- the same function the deferred legacy
// branch is built from, so a blended surface is lit like the opaque one behind it. The
// diffuse-only entry point: these objects have no specular map or colour to give one.
vec3 calcLegacyPointLightDiffuse(vec3 diffuse,
                    vec3 n, vec3 p, vec3 v,
                    vec3 lp, vec3 ld, vec3 lightColor,
                    float lightSize, float falloff, float is_pointlight);

#ifdef FS_FADE_IN
void fsFadeIn(); // fsFadeInF.glsl
#endif

// <FS:Trish> Soft particles (FSRenderSoftParticles)
#ifdef FS_SOFT_PARTICLES
float fsSoftParticleFade(vec3 pos); // fsSoftParticlesF.glsl
vec4 fsSoftParticleApply(vec4 color, float fade);
#endif
// </FS:Trish>

// <FS:Trish> Particle draws (fsparticlerender.cpp): set per draw of an in-world particle group, 0 for
// every other draw. Bit 1: the draw blends by source alpha and writes no depth, so a fragment the
// soft particle fade leaves below minimum_alpha adds nothing and stops before the lighting, as the
// minimum_alpha test does. Bit 2: cheap lighting (FSRenderCheapParticles).
uniform int fs_particle;
float fsSampleDirectionalShadowCheap(vec3 pos, vec3 norm); // shadowUtil.glsl
vec3 fsSampleProbeAmbientCheap(vec3 pos, vec3 norm, vec3 amblit); // reflectionProbeF.glsl
// </FS:Trish>

void main()
{
#ifdef FS_FADE_IN
    fsFadeIn();
#endif
    mirrorClip(vary_position);

    // <FS:Trish> The screen position from the window position: vary_fragcoord is clip space with
    // z (plus the near clip) in place of w, about 33 % off at 1 m. It feeds the sun shadow's jitter
    // and the screen-space reflection's edge fade and start. materialF already does this.
    //vec2 frag = vary_fragcoord.xy/vary_fragcoord.z*0.5+0.5;
    vec2 frag = gl_FragCoord.xy / screen_res;
    // </FS:Trish>

    vec4 pos = vec4(vary_position, 1.0);
#ifndef IS_AVATAR_SKIN
    // clip against water plane unless this is a legacy avatar skin
    waterClip(pos.xyz);
#endif
    // Interpolation across a triangle shortens a normal that was unit-length at each vertex.
    // Every sibling forward path normalizes here, and the deferred path always reads a unit
    // normal out of the GBuffer, so leaving it raw made this the one surface whose NdotL was
    // scaled by an accident of where it sat within its triangle.
    vec3 norm = normalize(vary_norm);

    float shadow = 1.0f;

#ifdef USE_DIFFUSE_TEX
    vec4 diffuse_tap = texture(diffuseMap,vary_texcoord0.xy);
#endif

#ifdef USE_INDEXED_TEX
    vec4 diffuse_tap = diffuseLookup(vary_texcoord0.xy);
#endif

    vec4 diffuse_srgb = diffuse_tap;

#ifdef FOR_IMPOSTOR
    vec4 color;
    color.rgb = diffuse_srgb.rgb;
    color.a = 1.0;

    float final_alpha = diffuse_srgb.a * vertex_color.a;
    diffuse_srgb.rgb *= vertex_color.rgb;

    // Insure we don't pollute depth with invis pixels in impostor rendering
    //
    if (final_alpha < minimum_alpha)
    {
        discard;
    }

    color.rgb = diffuse_srgb.rgb;
    color.a = final_alpha;

#else // FOR_IMPOSTOR

    // <FS:Trish> With a vertex colour the colour is decoded once below, after the tint
    //vec4 diffuse_linear = vec4(srgb_to_linear(diffuse_srgb.rgb), diffuse_srgb.a);
#ifdef USE_VERTEX_COLOR
    vec4 diffuse_linear = vec4(vec3(0.0), diffuse_srgb.a);
#else
    vec4 diffuse_linear = vec4(srgb_to_linear(diffuse_srgb.rgb), diffuse_srgb.a);
#endif
    // </FS:Trish>

    vec3 light_dir = (sun_up_factor == 1) ? sun_dir: moon_dir; // TODO -- factor out "sun_up_factor" and just send in the appropriate light vector

    float final_alpha = diffuse_linear.a;

// <FS:Trish> FSRenderAlphaRiggedNoDiscard: built without the alpha tests, for rigged draws whose texture has no
// alpha channel and whose face colour alpha is above minimum_alpha, which could never fail them. A program
// without discard lets the depth write go early.
#ifndef FS_NO_DISCARD
// </FS:Trish>
#ifdef IS_AVATAR_SKIN
    if(final_alpha < minimum_alpha)
    {
        discard;
    }
#endif
#endif // FS_NO_DISCARD <FS:Trish/>

#ifdef USE_VERTEX_COLOR
    final_alpha *= vertex_color.a;

#ifndef FS_NO_DISCARD // <FS:Trish/>
    if (final_alpha < minimum_alpha)
    { // TODO: figure out how to get invisible faces out of
        // render batches without breaking glow
        discard;
    }
#endif // FS_NO_DISCARD <FS:Trish/>

    diffuse_srgb.rgb *= vertex_color.rgb;
    diffuse_linear.rgb = srgb_to_linear(diffuse_srgb.rgb);
#endif // USE_VERTEX_COLOR

#ifdef FS_HAIR
    if ((fs_hair_pass == 1 && final_alpha < fs_hair_cut) || (fs_hair_pass == 2 && final_alpha >= fs_hair_cut))
    {
        discard;
    }
    if (fs_hair_pass == 3)
    { // depth only: the colour mask is off, skip the lighting
        frag_color = vec4(0.0);
        return;
    }
#endif

    // <FS:Trish> Particle draws: the soft fade is taken here instead of at the end (the same value),
    // so a particle fragment it fades out can leave before the lighting. Uniform per draw: coherent.
#ifdef FS_SOFT_PARTICLES
    float fs_soft_fade = fsSoftParticleFade(vary_position);
    if ((fs_particle & 1) != 0 && final_alpha * fs_soft_fade < minimum_alpha)
    {
        discard;
    }
#endif
    bool fs_cheap = (fs_particle & 2) != 0;
    // </FS:Trish>

    // After the alpha tests rather than ahead of them, so a fragment they reject does not pay
    // for the PCF taps. The shadow maps carry no mips, so nothing here depends on derivatives.
#ifdef HAS_SUN_SHADOW
    // <FS:Trish> shadow only enters as min(clamp(N.L), shadow), which is 0 facing away from the light
    //shadow = sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
    if (dot(norm.xyz, light_dir.xyz) > 0.0)
    {
        // cheap particles: one filtered tap, so fog in the shade stays in the shade
        shadow = fs_cheap ? fsSampleDirectionalShadowCheap(pos.xyz, norm.xyz) : sampleDirectionalShadow(pos.xyz, norm.xyz, frag);
    }
    // </FS:Trish>
#endif

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;

    calcAtmosphericVarsLinear(pos.xyz, norm, light_dir, sunlit, amblit, additive, atten);
    if (classic_mode > 0)
        sunlit *= 1.35;
    vec3 sunlit_linear = sunlit;
    vec3 amblit_linear = amblit;

    vec3 irradiance = amblit;
    vec3 glossenv = vec3(0.0);
    vec3 legacyenv = vec3(0.0);
    // <FS:Trish> Cheap particles: the sky probe's ambient alone, without the walk over the probes
    //sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag, pos.xyz, norm.xyz, 0.0, 0.0, true, amblit_linear);
    if (fs_cheap)
    {
        irradiance = fsSampleProbeAmbientCheap(pos.xyz, norm.xyz, amblit_linear);
    }
    else
    {
        sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, frag, pos.xyz, norm.xyz, 0.0, 0.0, true, amblit_linear);
    }
    // </FS:Trish>


    float da = dot(norm.xyz, light_dir.xyz);
          da = clamp(da, -1.0, 1.0);

    float final_da = da;
          final_da = clamp(final_da, 0.0f, 1.0f);

    vec4 color = vec4(0.0);

    color.a   = final_alpha;

    color.rgb = irradiance;
    if (classic_mode > 0)
    {
        final_da = pow(final_da,1.2);
        vec3 sun_contrib = vec3(min(final_da, shadow));

        color.rgb = srgb_to_linear(color.rgb * 0.9 + linear_to_srgb(sun_contrib) * sunlit_linear * 0.7);
        sunlit_linear = srgb_to_linear(sunlit_linear);
    }
    else
    {
        vec3 sun_contrib = min(final_da, shadow) * sunlit_linear;
        color.rgb += sun_contrib;
    }

    color.rgb *= diffuse_linear.rgb;

    vec4 light = vec4(0,0,0,0);

    vec3 npos = normalize(-pos.xyz);

// light_deferred_attenuation carries the size/falloff pair the deferred pass reads, as the PBR
// alpha path already takes them. light_attenuation.z stays: it is the is-omni flag.
   // <FS:Trish> A slot without a light (black, see LLPipeline::setupHWLights) adds exactly nothing:
   // skip it. The test is on a uniform, so every fragment takes the same branch.
   //#define LIGHT_LOOP(i) light.rgb += calcLegacyPointLightDiffuse(diffuse_linear.rgb, norm, pos.xyz, npos, light_position[i].xyz, light_direction[i].xyz, light_diffuse[i].rgb, light_deferred_attenuation[i].x, light_deferred_attenuation[i].y, light_attenuation[i].z);
   #define LIGHT_LOOP(i) if (any(notEqual(light_diffuse[i], vec3(0.0)))) { light.rgb += calcLegacyPointLightDiffuse(diffuse_linear.rgb, norm, pos.xyz, npos, light_position[i].xyz, light_direction[i].xyz, light_diffuse[i].rgb, light_deferred_attenuation[i].x, light_deferred_attenuation[i].y, light_attenuation[i].z); }
   // </FS:Trish>

    LIGHT_LOOP(1)
    LIGHT_LOOP(2)
    LIGHT_LOOP(3)
    LIGHT_LOOP(4)
    // <FS:Trish> Cheap particles: the four lights nearest the camera (the list is sorted by distance)
    if (!fs_cheap)
    {
    // </FS:Trish>
    LIGHT_LOOP(5)
    LIGHT_LOOP(6)
    LIGHT_LOOP(7)
    } // <FS:Trish/>

    // sum local light contrib in linear colorspace
    color.rgb += light.rgb;

    color.rgb = applySkyAndWaterFog(pos.xyz, additive, atten, color).rgb;

#endif // #else // FOR_IMPOSTOR
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;
#ifdef IS_HUD
    color.rgb = linear_to_srgb(color.rgb);
    final_scale = 1;
#endif

    color.rgb *= final_scale;
    // <FS:Trish> max() lets a NaN through on AMD (and keeps an infinity), and under WBOIT one such
    // fragment poisons the pixel's sums. Every finite value is kept as it was.
    //frag_color = max(color, vec4(0));
    frag_color = max(vec4(fsScrubNaN(color.rgb), color.a), vec4(0));
    // </FS:Trish>
    // <FS:Trish> Soft particles: fade where the particle meets the scene, before WBOIT weighs the alpha
#ifdef FS_SOFT_PARTICLES
#ifdef FOR_IMPOSTOR
    float fs_soft_fade = fsSoftParticleFade(vary_position); // no impostor program is built with it, kept whole
#endif
    //frag_color = fsSoftParticleApply(frag_color, fsSoftParticleFade(vary_position));
    frag_color = fsSoftParticleApply(frag_color, fs_soft_fade); // taken after the alpha tests
#endif
    // </FS:Trish>
#ifdef FS_WBOIT
    fs_wboit_weight = vec4(0.0);
    if (fs_wboit != 0)
    { // weight falls with the square of the view distance in metres (McGuire and Bavoil eq. 7)
        float wboit_a = clamp(frag_color.a, 0.0, 1.0);
        float wboit_z = 1.0 / gl_FragCoord.w;
        float wboit_w = wboit_a * clamp(1.0 / (1e-5 + pow(wboit_z / 5.0, 2.0) + pow(wboit_z / 200.0, 6.0)), 1e-3, 300.0);
        frag_color = vec4(min(frag_color.rgb, vec3(64.0)) * wboit_a * wboit_w, wboit_a);
        fs_wboit_weight = vec4(wboit_a * wboit_w, 0.0, 0.0, 0.0);
    }
#endif
}
