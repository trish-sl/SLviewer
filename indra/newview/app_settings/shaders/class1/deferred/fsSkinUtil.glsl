/**
 * @file fsSkinUtil.glsl
 *
 * Which avatar pixels are skin, shared by the skin scattering pass (fsSkinSSSF.glsl) and, with
 * the skin lighting options on, the deferred sun and local light passes.
 *
 * Avatar pixels: with FSRenderSkinSSS on, the rigged (HAS_SKIN) and system avatar (AVATAR_CLOTH)
 * g-buffer writers bias their flag by FS_SKIN_FLAG_BIAS (LLShaderMgr::loadShaderFile). Inside
 * that, a weight in [0, 1] says how much of the pixel is skin.
 *
 * What the g-buffer holds for an avatar pixel (mRT->deferredScreen):
 *  0 albedo    rgb: sRGB from legacy writers (avatarF, diffuseF, bumpF, materialF), linear from
 *              PBR (pbropaqueF). a: legacy fullbright share; 0 for PBR and the system avatar.
 *  1 specular  legacy: rgb specular colour (sRGB), a glossiness. materialF writes its specular
 *              map times colour and its glossiness; the shiny legacy writers (diffuseF, bumpF)
 *              their shininess level in all four; the system avatar (avatarF) zero.
 *              PBR: packORM, r occlusion, g + a roughness, b metallic.
 *  2 normal    xy octahedral normal. b: legacy environment intensity (materialF's environment
 *              factor, or the shininess level), PBR the geometric normal. a: the flag.
 *  3 emissive  not used here.
 * So the material is authored, and worth trusting, for PBR (metallic and roughness) and for
 * legacy materials (glossiness, environment, specular colour). The system avatar's bakes and
 * legacy mesh without a material carry none of it: there only the albedo tone tells skin.
 *
 * Material score (FSRenderSkinDetectMaterial, FS_SKIN_DETECT_MATERIAL): skin is a dielectric of
 * mid roughness. PBR: metallic near 0 and roughness roughly 0.25 to 0.8; most cloth is rougher,
 * jewellery and buckles metallic. Legacy: not mirror glossy, no strong environment reflection,
 * and no strongly tinted (metal coloured) specular. Pixels with no material information score
 * 1, so the tone alone decides there, as without the option.
 * It multiplies a relaxed tone score (fs_skin_tone_range): with the material doing the work of
 * keeping cloth out, the tone window can open up to pale, very dark and fantasy skins.
 *
 * Painted hair (FSRenderSkinHairFilter, fs_skin_hair_filter): a Bakes on Mesh hair base, brows
 * or lashes are painted into the head's bake, so they are the same surface as the skin and carry
 * no material of their own. Natural hair colours sit in skin's own hue band (both are melanin),
 * so colour alone cannot tell brown or blonde hair from skin; what gives it away is its strands,
 * fine light and dark lines that skin does not have (fsSkinHair). The weight the lighting and
 * the scattering use fades out there, so painted hair gets no sheen, skin highlight or red glow.
 */

/*[EXTRA_CODE_HERE]*/

uniform float sss_tone_match;       // 0: every avatar surface, 1: skin coloured albedo only
#ifdef FS_SKIN_DETECT_MATERIAL
uniform float fs_skin_tone_range;   // 0: the human tone window, 1: any hue, pale and dark
#endif
uniform float fs_skin_hair_filter;  // 0: painted hair counts as skin, 1: it does not (fsSkinHair)
uniform sampler2D diffuseRect;      // g-buffer albedo, for the strand test
uniform vec2 screen_res;

vec3 linear_to_srgb(vec3 cl);
vec3 srgb_to_linear(vec3 cs);
float unpackRoughness(vec2 p);
vec4 getNormRaw(vec2 screenpos);

#define FS_SKIN_FLAG_BIAS 0.05
#define FS_SKIN_MIN_ALBEDO 0.02     // linear; darker albedo is floored before dividing by it

// 0: not avatar, 1: legacy avatar surface, 2: PBR avatar surface
int fsSkinSurface(float flag)
{
    if (abs(flag - (GBUFFER_FLAG_HAS_ATMOS + FS_SKIN_FLAG_BIAS)) < 0.02)
    {
        return 1;
    }
    if (abs(flag - (GBUFFER_FLAG_HAS_PBR + FS_SKIN_FLAG_BIAS)) < 0.02)
    {
        return 2;
    }
    return 0;
}

// Hue in degrees (-180..180, red at 0), saturation and value of an sRGB colour
vec3 fsSkinHSV(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float s = d / max(mx, 1e-4);

    float h;
    if (d < 1e-4)
    {
        h = 180.0;
    }
    else if (mx == c.r)
    {
        h = 60.0 * (c.g - c.b) / d;
    }
    else if (mx == c.g)
    {
        h = 60.0 * ((c.b - c.r) / d + 2.0);
    }
    else
    {
        h = 60.0 * ((c.r - c.g) / d + 4.0);
        h = h > 180.0 ? h - 360.0 : h;
    }
    return vec3(h, s, mx);
}

// How much an albedo looks like skin, across the range of human tones: red leading, hue between
// red and yellow-orange, moderate saturation, not black. Smooth edges so the mask has no seams.
float fsSkinTone(vec3 c)
{
    vec3 hsv = fsSkinHSV(c);
    float wh = smoothstep(-25.0, -5.0, hsv.x) * (1.0 - smoothstep(45.0, 65.0, hsv.x));
    float ws = smoothstep(0.05, 0.14, hsv.y) * (1.0 - smoothstep(0.72, 0.88, hsv.y));
    float wv = smoothstep(0.04, 0.12, hsv.z);
    return wh * ws * wv;
}

#ifdef FS_SKIN_DETECT_MATERIAL
// fsSkinTone with its window opened by range: at 0 the same, at 1 any hue, any paleness, and
// down to near black; only neon saturation keeps some penalty
float fsSkinToneRelaxed(vec3 c, float range)
{
    vec3 hsv = fsSkinHSV(c);
    float r = clamp(range, 0.0, 1.0);
    float wh = smoothstep(-25.0, -5.0, hsv.x) * (1.0 - smoothstep(45.0, 65.0, hsv.x));
    float ws_pale = smoothstep(0.05, 0.14, hsv.y);
    float ws_neon = 1.0 - smoothstep(0.72, 0.88, hsv.y);
    float dark = 1.0 - 0.75 * r;
    float wv = smoothstep(0.04 * dark, 0.12 * dark, hsv.z);
    return mix(wh, 1.0, r) * mix(ws_pale, 1.0, r) * mix(ws_neon, 1.0, 0.5 * r) * wv;
}

// How much the stored material looks like skin (1 when the pixel carries no material)
float fsSkinMaterial(int surface, vec4 spec, float env)
{
    if (surface == 2)
    {
        float metallic = spec.b;
        float rough = unpackRoughness(spec.ga);
        return (1.0 - smoothstep(0.2, 0.5, metallic)) *
               smoothstep(0.12, 0.28, rough) * (1.0 - smoothstep(0.78, 0.92, rough));
    }

    // Legacy: mirror gloss or a strong environment reflection is not skin (the "low" shininess
    // some skins use stays in), nor a strongly tinted specular colour (gold, copper)
    float gloss = spec.a;
    float mx = max(spec.r, max(spec.g, spec.b));
    float mn = min(spec.r, min(spec.g, spec.b));
    float tint = (mx - mn) / max(mx, 1e-3);
    return (1.0 - smoothstep(0.7, 0.9, gloss)) *
           (1.0 - smoothstep(0.3, 0.6, env)) *
           (1.0 - smoothstep(0.35, 0.6, tint) * smoothstep(0.05, 0.2, mx));
}
#endif

// Skin weight of an avatar surface (surface from fsSkinSurface, not 0) in [0, 1], from its raw
// albedo, specular and environment texels. albedo: the linear diffuse albedo the lighting
// multiplied by, floored. material, tone: the two scores, for the debug view.
float fsSkinWeightParts(int surface, vec4 base, vec4 spec, float env, out vec3 albedo, out float material, out float tone)
{
    vec3 srgb;
    float w = 1.0;
    if (surface == 2)
    {
        albedo = base.rgb;
        srgb = linear_to_srgb(base.rgb);
    }
    else
    {
        albedo = srgb_to_linear(base.rgb);
        srgb = base.rgb;
        w = 1.0 - clamp(base.a, 0.0, 1.0); // legacy alpha is the fullbright share
    }
    albedo = max(albedo, vec3(FS_SKIN_MIN_ALBEDO));

#ifdef FS_SKIN_DETECT_MATERIAL
    material = fsSkinMaterial(surface, spec, env);
    tone = fsSkinToneRelaxed(srgb, fs_skin_tone_range);
#else
    material = 1.0;
    tone = fsSkinTone(srgb);
#endif
    return w * material * mix(1.0, tone, clamp(sss_tone_match, 0.0, 1.0));
}

float fsSkinWeightRaw(int surface, vec4 base, vec4 spec, float env, out vec3 albedo)
{
    float material;
    float tone;
    return fsSkinWeightParts(surface, base, spec, env, albedo, material, tone);
}

// Brightness of a stored albedo on a perceptual scale: legacy stores sRGB, PBR linear, for which
// the square root stands in for the sRGB curve
float fsSkinPerceptualLum(int surface, vec3 stored)
{
    float y = dot(stored, vec3(0.2126, 0.7152, 0.0722));
    return surface == 2 ? sqrt(max(y, 0.0)) : y;
}

// One neighbour of the strand test: its contrast to the centre's brightness lc, counted only on
// the avatar (the background or clothing beside it is not texture)
void fsSkinHairTap(vec2 tc, float lc, inout float sum, inout float n)
{
    int surface = fsSkinSurface(getNormRaw(tc).a);
    if (surface != 0)
    {
        float l = fsSkinPerceptualLum(surface, texture(diffuseRect, tc).rgb);
        sum += abs(l - lc) / (l + lc + 0.04);
        n += 1.0;
    }
}

// How much an avatar pixel looks like painted hair rather than skin, in [0, 1]. surface from
// fsSkinSurface (not 0), stored its g-buffer albedo as stored. Two signs:
// - Strands: the mean contrast to the albedo 2 pixels away on each side (Michelson, on a
//   perceptual scale). Skin, pores and mottling included, stays under about 0.03; painted hair,
//   with its strands and streaks, sits well above, from the bake's full size down to a head
//   about a hundred pixels tall. Colours unlike skin (near grey, off the skin hue band, neon)
//   need less of it, so a grey, black or dyed hair base goes out sooner, while flat fantasy
//   skin, with no strands, stays skin.
// - Near black: no skin is this dark (the darkest human tones are about 0.16 in sRGB), black hair
//   and lashes are. Without this, a dark paint takes the sheen and highlight, whose strength does
//   not follow the albedo, about as bright as its own colour: the glossy look.
// The thresholds were checked against skin tones from very pale to very dark (the Monk scale),
// natural and dyed hair colours, and synthetic skin and hair textures at several distances.
float fsSkinHair(vec2 tc, int surface, vec3 stored)
{
    vec3 srgb = surface == 2 ? linear_to_srgb(stored) : stored;
    vec3 hsv = fsSkinHSV(srgb);
    float dark = 1.0 - smoothstep(0.06, 0.13, hsv.z);

    float grey = (1.0 - smoothstep(0.04, 0.09, hsv.y)) * (1.0 - smoothstep(0.70, 0.85, hsv.z));
    float wh = smoothstep(-25.0, -5.0, hsv.x) * (1.0 - smoothstep(45.0, 65.0, hsv.x));
    float off_hue = (1.0 - wh) * smoothstep(0.08, 0.16, hsv.y); // a hue only with some colour
    float neon = smoothstep(0.72, 0.88, hsv.y);
    float unlike = max(grey, max(off_hue, neon));

    float lc = fsSkinPerceptualLum(surface, stored);
    vec2 px = 2.0 / screen_res;
    float sum = 0.0;
    float n = 0.0;
    fsSkinHairTap(tc + vec2(px.x, 0.0), lc, sum, n);
    fsSkinHairTap(tc - vec2(px.x, 0.0), lc, sum, n);
    fsSkinHairTap(tc + vec2(0.0, px.y), lc, sum, n);
    fsSkinHairTap(tc - vec2(0.0, px.y), lc, sum, n);
    float detail = n > 0.0 ? sum / n : 0.0;
    float strands = smoothstep(0.035, 0.09, detail * (1.0 + unlike));
    float hair = max(dark, strands);
    return (isnan(hair) || isinf(hair)) ? 0.0 : clamp(hair, 0.0, 1.0);
}

// What the painted hair test leaves of a pixel's skin weight: 1 with the filter off
float fsSkinHairKeepFrom(vec2 tc, int surface, vec3 stored)
{
    float f = clamp(fs_skin_hair_filter, 0.0, 1.0);
    if (f <= 0.0)
    {
        return 1.0;
    }
    return 1.0 - f * fsSkinHair(tc, surface, stored);
}

// The same for the g-buffer pixel at tc (1 when it is not an avatar surface)
float fsSkinHairKeep(vec2 tc)
{
    int surface = fsSkinSurface(getNormRaw(tc).a);
    if (surface == 0 || fs_skin_hair_filter <= 0.0)
    {
        return 1.0;
    }
    return fsSkinHairKeepFrom(tc, surface, texture(diffuseRect, tc).rgb);
}

// Skin weight of a g-buffer pixel at tc for the lighting passes: 0 when not avatar or not skin
float fsSkinWeightGB(GBufferInfo gb, vec2 tc)
{
    int surface = fsSkinSurface(gb.gbufferFlag);
    if (surface == 0)
    {
        return 0.0;
    }
    vec3 albedo;
    float w = clamp(fsSkinWeightRaw(surface, gb.albedo, gb.specular, gb.envIntensity, albedo), 0.0, 1.0);
    if (w > 0.0)
    {
        w *= fsSkinHairKeepFrom(tc, surface, gb.albedo.rgb);
    }
    return w;
}

#ifdef FS_SKIN_DUAL_SPEC
// Two-lobe skin specular (FSRenderSkinDualSpecular). Skin's oily surface film gives a sharp
// highlight, its fine wrinkles and pores a broad sheen around it; one GGX lobe can show only one
// of them. After Jimenez et al. / Activision's character shading: a sharp lobe at 0.75 x the
// material's roughness and a broad one at 1.75 x, mixed fs_skin_spec_mix (0.85) to the rest.
// Each GGX lobe is normalised, so the mix reflects the same energy as one lobe would, at skin's
// measured F0 of 0.028 (index of refraction about 1.4) in place of the generic dielectric 0.04.
uniform float fs_skin_spec_mix;     // share of the sharp lobe
#define FS_SKIN_F0 0.028

// F * Vis * D of one GGX lobe, term for term pbrPunctual's (deferredUtil.glsl): Schlick Fresnel
// to 1 at grazing, height-correlated Smith visibility, Trowbridge-Reitz distribution. Same
// clamps, so the stock lobe it stands for comes out the same.
vec3 fsSkinGGX(vec3 n, vec3 v, vec3 l, float perceptualRoughness, vec3 f0)
{
    float r = max(perceptualRoughness, 0.045); // MIN_PBR_ROUGHNESS
    float a = r * r;
    float a2 = a * a;
    vec3 h = normalize(l + v);
    float nl = clamp(dot(n, l), 0.0, 1.0);
    float nv = clamp(abs(dot(n, v)), 0.001, 1.0);
    float nh = clamp(dot(n, h), 0.0, 1.0);
    float vh = clamp(dot(v, h), 0.0, 1.0);

    float f = clamp(1.0 - vh, 0.0, 1.0);
    float f2 = f * f;
    vec3 fresnel = f0 + (vec3(1.0) - f0) * (f2 * f2 * f);

    float lambda_v = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float lambda_l = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    float lambda = lambda_v + lambda_l;
    float vis = lambda > 0.0 ? 0.5 / lambda : 0.0;

    float d = (nh * a2 - nh) * nh + 1.0;
    float dist = a2 / (3.14159265 * d * d);
    return fresnel * (vis * dist);
}

// The two-lobe skin specular BRDF (no N.L) for a material roughness
vec3 fsSkinDualLobe(vec3 n, vec3 v, vec3 l, float perceptualRoughness)
{
    float r = clamp(perceptualRoughness, 0.0, 1.0);
    float m = clamp(fs_skin_spec_mix, 0.0, 1.0);
    vec3 f0 = vec3(FS_SKIN_F0);
    return m * fsSkinGGX(n, v, l, r * 0.75, f0) + (1.0 - m) * fsSkinGGX(n, v, l, min(r * 1.75, 1.0), f0);
}

// Roughness for legacy skin: from the glossiness where the material has one, otherwise (system
// avatar bakes, legacy mesh without a material) a typical skin value
float fsSkinLegacyRoughness(vec4 spec)
{
    return spec.a > 0.0 ? clamp(1.0 - spec.a, 0.3, 0.9) : 0.55;
}

// Skin specular on a legacy-shaded pixel from one light, per unit of the light's colour times its
// attenuation, scaled as the PBR path scales its lobe (N.L, PUNCTUAL_LIGHT_SCALE), so legacy and
// PBR skin shine alike. Replaces the Blinn-Phong lobe, which the caller scales by 1 - weight.
vec3 fsSkinLegacySpecular(vec3 n, vec3 v, vec3 l, vec4 spec)
{
    float nl = clamp(dot(n, l), 0.0, 1.0);
    return fsSkinDualLobe(n, v, l, fsSkinLegacyRoughness(spec)) * (nl * PUNCTUAL_LIGHT_SCALE);
}
#endif

#ifdef FS_SKIN_WARM_SHADOWS
// Warm occlusion and shadow edges on skin (FSRenderSkinWarmShadows). Light that enters skin
// travels under it before it leaves, red furthest, so a crease, an eye socket or the edge of a
// shadow still receives red light from the lit skin around it: on skin they go warm, not grey.
// A visibility v in [0, 1] (ambient occlusion, or the sun shadow in its penumbra) becomes v^e
// per channel, e below 1 and lowest for red, following how far each channel reaches in the skin
// profile. Fully lit (1) and fully dark (0) stay as they are; only the range in between is
// lifted, red the most. The coloured multi-bounce occlusion of Jimenez et al. 2016 ("Practical
// Real-Time Strategies for Accurate Indirect Occlusion"), taken toward skin's subsurface colour,
// and the per channel shadow softening used for skin penumbrae.
uniform float fs_skin_warm;         // strength: 0 grey as without the option, 1 strongly warm

vec3 fsSkinWarmVisibility(vec3 v, float w)
{
    vec3 e = vec3(1.0) - clamp(fs_skin_warm, 0.0, 1.0) * clamp(w, 0.0, 1.0) * vec3(0.6, 0.25, 0.1);
    return pow(clamp(v, vec3(0.0), vec3(1.0)), e);
}
#endif

// Transmittance of the skin profile through s mm (Jimenez et al. 2010): the same six Gaussians,
// so red carries through far more skin than green and blue
vec3 fsSkinTransmittance(float s)
{
    float d = -s * s;
    return vec3(0.233, 0.455, 0.649) * exp(d / 0.0064) +
           vec3(0.100, 0.336, 0.344) * exp(d / 0.0484) +
           vec3(0.118, 0.198, 0.000) * exp(d / 0.187) +
           vec3(0.113, 0.007, 0.007) * exp(d / 0.567) +
           vec3(0.358, 0.004, 0.000) * exp(d / 1.99) +
           vec3(0.078, 0.000, 0.000) * exp(d / 7.41);
}

// How much of the light coming through the skin heads for the eye (n: normal, v: toward the eye,
// l: toward the light). Thin skin scatters mostly forward, so the glow is strongest looking into
// the light through it; bending the light direction by the normal gathers that on the rims of an
// ear or a finger rather than lighting the whole part evenly. About 0.35 seen from the side,
// up to 2 looking straight into the light.
float fsSkinForwardScatter(vec3 n, vec3 v, vec3 l)
{
    vec3 h = l + n * 0.4;
    float f = clamp(dot(v, -h) * inversesqrt(max(dot(h, h), 1e-8)), 0.0, 1.0);
    return 0.35 + 1.65 * f * f * f;
}

// Skin seen edge on is the thin edge of a part (the rim of an ear, the side of a finger): the
// thickness there is scaled down, by up to 40 % at the silhouette, so the glow grades toward it
float fsSkinRimThinning(vec3 n, vec3 v)
{
    float g = 1.0 - abs(dot(n, v));
    return 1.0 - 0.4 * g * g;
}

#ifdef FS_SKIN_TRANS_LOCAL
// Light from local lights shining through thin skin from behind (FSRenderSkinTranslucencyLocal):
// ears, fingers and nostril rims glow red with a club light or a lamp behind them. Point lights
// have no shadow maps in SL, so the thickness the light passes through cannot be measured as the
// sun's translucency measures it (fsSkinSSSF.glsl). It is estimated from how strongly the surface
// bends instead: a thin rounded part of radius R (an ear rim, a finger) bends by 1/R and is about
// 2R thick, while a cheek or the back of a head bends gently and is thick, so it stays dark. The
// bend is how fast the stored normal turns: the angle between the normals a few pixels either
// side over the distance between their positions, on both screen axes, taking the stronger.
// The normals are interpolated across the triangles, so the bend is smooth over a faceted mesh
// (the depth buffer's own curvature is zero inside each flat triangle and spikes on its edges,
// which drew the triangle edges as red lines). Across a silhouette or onto another surface an
// axis is skipped; with neither usable, the part counts as thick.
uniform float sss_translucency;     // strength, FSRenderSkinSSSTranslucency
uniform float sss_radius;           // profile scale, 1 = measured human skin
uniform float sss_tint;             // 0: every channel transmits like red, 1: skin profile
vec4 getPosition(vec2 pos_screen);
vec3 getGeoNorm(vec2 screenpos);

#define FS_SKIN_TRANS_MM 6.0       // as fsSkinSSSF.glsl: measured thickness (mm) per profile mm
#define FS_SKIN_THICK_MAX_MM 200.0  // flat or unknown: no light gets through

// Estimated thickness in mm of the skin at tc (view space position pos), thinned toward the
// silhouette (fsSkinRimThinning). The bend and the rim are measured on the surface's own normal,
// without its normal map (getGeoNorm, which PBR surfaces store): a skin normal map's wrinkles and
// pores turn the shading normal sharply over a few pixels, which read as thin skin and glowed as
// red stripes along them, worst on a cheek seen at a grazing angle. The stored geometric normal is
// quantized to about 1/128 rad at most, which reads as tens of centimetres of skin, so it adds no
// glow. Legacy surfaces store none and get their shading normal back from getGeoNorm, as before.
// n_shading is unused (the shading normal the callers have).
float fsSkinThickness(vec2 tc, vec3 pos, vec3 n_shading)
{
    vec3 n = getGeoNorm(tc);
    float tol = 0.02 * max(-pos.z, 0.05); // a bigger depth step is another surface
    float bend = 0.0;
    for (int axis = 0; axis < 2; ++axis)
    {
        vec2 d = (axis == 0 ? vec2(3.0, 0.0) : vec2(0.0, 3.0)) / screen_res;
        vec3 pa = getPosition(tc + d).xyz;
        vec3 pb = getPosition(tc - d).xyz;
        if (abs(pa.z - pos.z) > tol || abs(pb.z - pos.z) > tol)
        {
            continue;
        }
        vec3 na = getGeoNorm(tc + d);
        vec3 nb = getGeoNorm(tc - d);
        if (dot(na, n) < 0.0 || dot(nb, n) < 0.0)
        {
            continue; // folded onto another part
        }
        // On a circle of radius R, normals at arc s either side differ by 2 sin(s / R), and the
        // points by a chord of about 2s, so the ratio is the bend 1 / R
        bend = max(bend, length(na - nb) / max(distance(pa, pb), 1e-6));
    }
    float thickness = bend > 0.0 ? 2000.0 / bend : FS_SKIN_THICK_MAX_MM;
    if (!(thickness < FS_SKIN_THICK_MAX_MM))
    {
        return FS_SKIN_THICK_MAX_MM; // flat or unknown: the callers skip it
    }
    vec3 v = -pos * inversesqrt(max(dot(pos, pos), 1e-12));
    return max(thickness, 1.0) * fsSkinRimThinning(n, v);
}

// Light a local light behind the skin sends through it to the eye, per unit of the light's colour
// times its attenuation: the skin profile's transmittance through the thickness, on the albedo,
// as strong as the light falls on the far side (N.L there, 0 at the terminator)
vec3 fsSkinLocalTranslucency(vec3 albedo, vec3 n, vec3 l, float thickness_mm, float w, vec3 pos)
{
    float back = -dot(n, l);
    if (back <= 0.0 || w <= 0.0 || thickness_mm >= FS_SKIN_THICK_MAX_MM)
    {
        return vec3(0.0);
    }
    vec3 v = -pos * inversesqrt(max(dot(pos, pos), 1e-12));
    // (the rim thinning is already in thickness_mm, on the smooth normal: fsSkinThickness)
    vec3 t = fsSkinTransmittance(thickness_mm / (FS_SKIN_TRANS_MM * max(sss_radius, 0.05)));
    t = mix(vec3(t.r), t, clamp(sss_tint, 0.0, 1.0));
    return t * albedo * (min(back, 1.0) * fsSkinForwardScatter(n, v, l) * clamp(sss_translucency, 0.0, 4.0) * w);
}

// The linear diffuse albedo of a g-buffer pixel
vec3 fsSkinAlbedo(GBufferInfo gb)
{
    return GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR) ? gb.albedo.rgb : srgb_to_linear(gb.albedo.rgb);
}
#endif

#ifdef FS_SKIN_SHEEN
// Soft rim sheen (FSRenderSkinSheen): the fine vellus hair on skin (peach fuzz) catches light at
// grazing angles, a faint soft glow along the silhouette. The sun's part is the Charlie sheen
// lobe (Estevez and Kulla 2017, "Production Friendly Microfacet Sheen BRDF") with Neubelt's
// visibility, as glTF's KHR_materials_sheen has it; the ambient part a rim, rising toward
// grazing, on the ambient light. Its colour is the skin's own hue, lightened, times
// fs_skin_sheen.
// Both fade out over the last stretch to the silhouette (fsSkinSheenEdge). Neubelt's visibility
// does not fall to zero at grazing as the full Charlie visibility does: N.L times it stays at a
// quarter all along the silhouette, up to the terminator. And at a silhouette the interpolated
// normal often already faces away from the eye, which the rim read as fully grazing. Together
// they drew a hard, full strength line around every outline, including the scalp's under mesh
// hair, which showed through the hair's fine strands as a bright edge around the hair.
uniform float fs_skin_sheen;        // strength
#define FS_SKIN_SHEEN_ALPHA 0.25    // sheen roughness 0.5, squared

vec3 fsSkinSheenColor(vec3 albedo)
{
    float mx = max(albedo.r, max(albedo.g, albedo.b));
    return mix(vec3(1.0), albedo / max(mx, 1e-3), 0.5) * clamp(fs_skin_sheen, 0.0, 1.0);
}

// 0 where the normal faces away from the eye, rising to 1 by N.V 0.15 (about 81 degrees off
// face on): on a rounded part that is the outermost pixel or two of its outline
float fsSkinSheenEdge(float nv)
{
    return smoothstep(0.0, 0.15, nv);
}

// Charlie distribution times Neubelt visibility (no N.L), faded at the silhouette
float fsSkinSheenBRDF(vec3 n, vec3 v, vec3 l)
{
    vec3 h = normalize(l + v);
    float nh = clamp(dot(n, h), 0.0, 1.0);
    float nl = clamp(dot(n, l), 0.0, 1.0);
    float nv_raw = dot(n, v);
    float nv = clamp(abs(nv_raw), 0.001, 1.0);
    float inv_a = 1.0 / FS_SKIN_SHEEN_ALPHA;
    float sin2h = max(1.0 - nh * nh, 0.0078125);
    float d = (2.0 + inv_a) * pow(sin2h, inv_a * 0.5) / (2.0 * 3.14159265);
    float vis = 1.0 / (4.0 * max(nl + nv - nl * nv, 1e-3));
    return d * vis * fsSkinSheenEdge(nv_raw);
}

// How much of the ambient light the sheen returns at view angle nv: about nothing face on, rising
// toward grazing to about a seventh, then fading at the silhouette itself
float fsSkinSheenRim(float nv)
{
    float f = 1.0 - clamp(nv, 0.0, 1.0);
    return 0.25 * f * f * f * fsSkinSheenEdge(nv);
}
#endif
