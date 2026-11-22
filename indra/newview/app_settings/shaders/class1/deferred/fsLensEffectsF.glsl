// Lens effects on the finished image: chromatic aberration, lens flare and film grain

/*[EXTRA_CODE_HERE]*/

// Runs on the display referred image, after tone mapping and anti-aliasing.

out vec4 frag_color;

in vec2 vary_fragcoord;
// How much of the sun's disc is not covered by geometry, the same for every pixel: taken once by
// fsLensEffectsV.glsl
flat in float vary_sun_vis;

uniform sampler2D diffuseRect;
uniform sampler2D depthMap;
uniform mat4 inv_proj;
uniform vec2 screen_res;

// Chromatic aberration: red and blue pulled apart along the line from the centre, by this many
// pixels at the corners
uniform float ca_pixels;
// Depth of field: when ca_dof is set the split also follows how far out of focus the pixel is,
// using the same circle of confusion as the depth of field pass
uniform int ca_dof;
uniform float focal_distance;
uniform float blur_constant;
uniform float tan_pixel_angle;
uniform float magnification;
uniform float max_cof;

// Lens flare
uniform float flare_strength;
uniform vec3 flare_color;
uniform vec3 sun_screen;        // xy in 0..1, z 1 when the sun is in front of the camera
uniform float flare_ghosts;     // 1 for the sun; the moon is mostly glow, too dim for ghosts

// High quality lens flare (FSRenderLensFlareHQ), in place of the one above when flare_hq is set.
// Its strength comes from the HDR scene: how bright the uncovered part of the sun's disc is after
// exposure (fsLensFlareStateF), and the brightest small lights on screen (fsLensFlarePickF and
// fsLensFlareSelectF). Ghosts are images of a polygonal aperture with coloured coatings, each
// colour a slightly different size; a ring of dispersed colours lies around the sun; and the
// starburst is the aperture's diffraction spikes with fine streaks between them.
uniform int flare_hq;
uniform sampler2D sceneMap;     // the sun's drive, 1 x 1 (r)
uniform sampler2D bloomMap;     // the lights kept, 8 x 2: row 0 uv, strength; row 1 colour
uniform sampler2D exposureMap;
uniform float exposure;         // RenderExposure, as the tone mapper clamps it
uniform vec4 flare_hq_params;   // x starburst strength, y light flare strength (0 none this frame), z aperture blades (0 round), w sun disc radius (screen heights)
uniform float flare_hq_sun;     // 1 when the sun's drive was measured this frame

// Film grain
uniform float grain_strength;
uniform float grain_size;       // in pixels
uniform float grain_seed;       // changes every frame

// FSRenderMergePostPasses: the vignette pass (exoVignetteF) in this one. Its setting, x 0 for none; the levels
// of the targets the two passes stored the image in, x after this pass, y after the vignette (255 for 8 bit,
// 0 for none or float), so it rounds as they did
uniform vec3 vignette;
uniform vec2 vignette_quantize;

float viewDepth(vec2 tc)
{
#ifdef REVERSE_Z
    float z = texture(depthMap, tc).r; // the NDC depth itself
#else
    float z = texture(depthMap, tc).r * 2.0 - 1.0;
#endif
    vec4 p = inv_proj * vec4(0.0, 0.0, z, 1.0);
    return p.z / p.w;
}

// How far out of focus, 0 sharp to 1 at the largest blur the depth of field draws
float defocus(vec2 tc)
{
    float depth = viewDepth(tc);
    float sc = (depth - focal_distance) / -depth * blur_constant;
    sc /= magnification;
    float pixel_length = tan_pixel_angle * -focal_distance;
    sc = sc / pixel_length * 1.414;
    return clamp(abs(sc) / max(max_cof, 1e-4), 0.0, 1.0);
}

vec3 chromaticAberration(vec2 tc, vec3 base)
{
    vec2 from_center = tc - 0.5;
    // Grows with distance from the centre, as lateral aberration does in a real lens
    vec2 shift = from_center * 2.0 * ca_pixels / screen_res;
    if (ca_dof != 0)
    {
        shift *= defocus(tc);
    }
    if (dot(shift, shift) < 1e-10)
    {
        return base;
    }
    float r = texture(diffuseRect, tc + shift).r;
    float b = texture(diffuseRect, tc - shift).b;
    return vec3(r, base.g, b);
}

vec3 lensFlare(vec2 tc)
{
    vec2 sun = sun_screen.xy;
    float vis = vary_sun_vis;
    if (vis <= 0.0)
    {
        return vec3(0.0);
    }

    float aspect = screen_res.x / screen_res.y;
    vec2 to_px = (tc - sun) * vec2(aspect, 1.0);
    float d_sun = length(to_px);

    // Glare around the light itself, with faint spikes from the aperture blades
    float glare = exp(-d_sun * 18.0) * 0.6 + exp(-d_sun * 4.0) * 0.08;
    float ang = atan(to_px.y, to_px.x);
    float spikes = pow(abs(cos(ang * 3.0)), 60.0) * exp(-d_sun * 6.0) * 0.25;

    vec3 flare = vec3(glare + spikes * flare_ghosts);

    // Ghosts: reflections between lens elements land on the line from the sun through the
    // centre of the frame, on both sides of it, each a soft disc with its own coating tint
    vec2 axis = vec2(0.5) - sun;
    const int GHOSTS = 5;
    float positions[GHOSTS] = float[](0.45, 0.8, 1.25, 1.6, 2.1);
    float radii[GHOSTS] = float[](0.035, 0.07, 0.025, 0.11, 0.05);
    vec3 tints[GHOSTS] = vec3[](vec3(0.9, 0.6, 1.0), vec3(0.5, 0.8, 1.0), vec3(1.0, 0.8, 0.5),
                                vec3(0.6, 1.0, 0.7), vec3(1.0, 0.6, 0.6));
    for (int i = 0; i < GHOSTS; ++i)
    {
        vec2 center = sun + axis * positions[i];
        float d = length((tc - center) * vec2(aspect, 1.0));
        float disc = 1.0 - smoothstep(radii[i] * 0.7, radii[i], d);
        flare += tints[i] * disc * (0.07 * flare_ghosts);
    }

    // A faint ring around the centre of the frame, on the side facing the sun
    float ring_d = length((tc - 0.5) * vec2(aspect, 1.0));
    float ring = smoothstep(0.02, 0.0, abs(ring_d - 0.42));
    float facing = max(dot(normalize((tc - 0.5) * vec2(aspect, 1.0) + 1e-5), normalize(-axis * vec2(aspect, 1.0) + 1e-5)), 0.0);
    flare += vec3(0.7, 0.85, 1.0) * ring * facing * (0.04 * flare_ghosts);

    return flare * flare_color * vis * flare_strength;
}

float hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// ---- High quality lens flare --------------------------------------------------------------------
// Everything below works in screen heights around the frame centre, so shapes stay round on any
// window, and returns light in exposed linear units.

const float FS_TAU = 6.2831853;
const float FS_APERTURE_ROTATION = 0.3; // the blades sit a little off square to the frame

// Distance from the centre to the edge of a regular polygon with the given number of sides, at
// this angle, as a share of its corner radius
float fsApertureEdge(float angle, float blades)
{
    float seg = FS_TAU / blades;
    float a = mod(angle + FS_APERTURE_ROTATION, seg) - 0.5 * seg;
    return cos(0.5 * seg) / cos(a);
}

// One ghost, an image of the aperture: a soft polygon whose rim is brighter than its middle, as
// the coatings reflect most near the edge, with red drawn a little larger than blue
vec3 fsGhost(vec2 p, vec2 centre, float radius, float dispersion, float blades)
{
    vec2 q = p - centre;
    float d = length(q);
    if (d > radius * (1.0 + dispersion) * 1.05)
    {
        return vec3(0.0);
    }
    float edge = blades >= 3.0 ? fsApertureEdge(atan(q.y, q.x), blades) : 1.0;
    vec3 x = d / (radius * edge * vec3(1.0 + dispersion, 1.0, 1.0 - dispersion));
    vec3 body = 1.0 - smoothstep(vec3(0.86), vec3(1.0), x);
    vec3 rim = smoothstep(vec3(0.55), vec3(0.97), x) * body;
    return body * 0.5 + rim * 0.5;
}

// Diffraction spikes from the aperture's straight edges and the fine streaks between them. An
// even number of blades makes as many spikes, an odd number twice as many.
vec3 fsStarburst(vec2 q, float len, float blades, float core)
{
    float r = length(q);
    if (r > len * 8.0 || len <= 0.0)
    {
        return vec3(0.0);
    }
    float angle = atan(q.y, q.x) + FS_APERTURE_ROTATION;
    float n = mod(blades, 2.0) > 0.5 ? blades * 2.0 : blades;
    // A round aperture (fewer than 3 blades) has no straight edges, so no spikes, only the streaks
    float spikes = blades >= 3.0 ? pow(abs(cos(angle * n * 0.5)), 400.0 / n) : 0.0;

    // 96 streaks around the circle, each with its own brightness and reach
    const float STREAKS = 96.0;
    float s = fract(angle / FS_TAU) * STREAKS;
    float i0 = floor(s);
    float h0 = hash(vec2(i0, 7.0));
    float h1 = hash(vec2(mod(i0 + 1.0, STREAKS), 7.0));
    float streak = mix(h0, h1, smoothstep(0.0, 1.0, s - i0));
    streak *= streak * streak;

    // Longer wavelengths spread further, so the tips go red
    vec3 reach = vec3(1.1, 1.0, 0.88);
    vec3 spike_fall = exp(-r / (len * reach));
    vec3 streak_fall = exp(-r / (len * (0.35 + 0.65 * streak) * 0.6 * reach));
    float core_fade = smoothstep(0.0, max(core, 1e-4), r);
    return (spikes * spike_fall + streak * streak_fall * 0.4) * core_fade;
}

// A thin ring of dispersed colour around the light, brightest on the side toward the frame centre
vec3 fsHaloRing(vec2 q, vec2 toward_centre, float radius, float width)
{
    float d = length(q);
    vec3 off = (d - radius * vec3(1.03, 1.0, 0.97)) / width;
    vec3 ring = exp(-off * off);
    float facing = d > 1e-5 ? dot(q / d, toward_centre) : 0.0;
    return ring * (0.45 + 0.55 * max(facing, 0.0));
}

vec3 fsSunFlareHQ(vec2 p, vec2 sun, float drive)
{
    float blades = flare_hq_params.z;
    vec2 q = p - sun;
    float r = length(q);
    vec2 axis = -sun; // from the sun through the frame centre, which is at 0
    float axis_len = length(axis);
    vec2 toward_centre = axis_len > 1e-5 ? axis / axis_len : vec2(0.0);

    // Glare close around the sun
    vec3 flare = vec3(exp(-r / 0.025) * 0.5 + exp(-r / 0.12) * 0.05);

    // Brighter suns make larger ghosts, as more of each one clears the visibility threshold
    float grow = 0.9 + 0.15 * drive;
    const int GHOSTS = 7;
    float place[GHOSTS] = float[](0.32, 0.6, 0.85, 1.18, 1.45, 1.8, 2.25);
    float size[GHOSTS] = float[](0.025, 0.055, 0.016, 0.085, 0.035, 0.05, 0.13);
    float bright[GHOSTS] = float[](0.07, 0.035, 0.09, 0.022, 0.05, 0.03, 0.015);
    vec3 coat[GHOSTS] = vec3[](vec3(0.85, 0.6, 1.0), vec3(0.45, 0.8, 1.0), vec3(1.0, 0.8, 0.45),
                               vec3(0.55, 1.0, 0.65), vec3(1.0, 0.55, 0.5), vec3(0.6, 0.7, 1.0), vec3(0.9, 0.85, 0.7));
    vec3 ghosts = vec3(0.0);
    for (int i = 0; i < GHOSTS; ++i)
    {
        vec2 centre = sun + axis * place[i];
        float dispersion = 0.025 + 0.03 * abs(place[i] - 1.0);
        ghosts += coat[i] * fsGhost(p, centre, size[i] * grow, dispersion, blades) * bright[i];
    }
    flare += ghosts * flare_ghosts;

    flare += fsHaloRing(q, toward_centre, 0.32, 0.012) * (0.03 * flare_ghosts);
    flare += fsStarburst(q, 0.09 * grow, blades, flare_hq_params.w) * (0.35 * flare_hq_params.x);

    return flare * drive;
}

// The smaller flare of a bright light on screen: spikes, a little glare and two faint ghosts
vec3 fsLightFlareHQ(vec2 p, vec2 light, float a)
{
    float blades = flare_hq_params.z;
    vec2 q = p - light;
    float r = length(q);
    vec3 flare = vec3(exp(-r / 0.006) * 0.25);
    flare += fsStarburst(q, 0.012 + 0.04 * a, blades, 0.004) * (0.4 * flare_hq_params.x);
    vec2 axis = -light;
    flare += fsGhost(p, light + axis * 0.7, 0.012 + 0.01 * a, 0.03, blades) * 0.04;
    flare += fsGhost(p, light + axis * 1.4, 0.02 + 0.015 * a, 0.04, blades) * 0.025;
    return flare * a;
}

// The whole flare at this pixel, exposed linear
vec3 fsLensFlareHQ(vec2 tc)
{
    float aspect = screen_res.x / max(screen_res.y, 1.0);
    vec2 p = (tc - 0.5) * vec2(aspect, 1.0);
    vec3 flare = vec3(0.0);

    float drive = texelFetch(sceneMap, ivec2(0, 0), 0).r * flare_hq_sun;
    if (drive > 0.0 && sun_screen.z > 0.5)
    {
        vec2 sun = (sun_screen.xy - 0.5) * vec2(aspect, 1.0);
        flare += fsSunFlareHQ(p, sun, drive) * flare_color;
    }

    if (flare_hq_params.y > 0.0)
    {
        float scale = max(exposure * texture(exposureMap, vec2(0.5)).r, 1e-4);
        for (int k = 0; k < 8; ++k)
        {
            vec4 light = texelFetch(bloomMap, ivec2(k, 0), 0);
            if (light.z <= 0.0)
            {
                continue;
            }
            // Strength after exposure, eased so a huge neon sign is not a hundred times a lamp
            float e = light.z * scale;
            float a = e / (e + 2000.0);
            vec3 color = texelFetch(bloomMap, ivec2(k, 1), 0).rgb;
            flare += fsLightFlareHQ(p, (light.xy - 0.5) * vec2(aspect, 1.0), a) * color * flare_hq_params.y;
        }
    }
    return flare * flare_strength;
}

vec3 fsToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 fsToDisplay(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 filmGrain(vec2 tc, vec3 c)
{
    vec2 cell = floor(tc * screen_res / max(grain_size, 1.0));
    // Two hashes give a roughly bell shaped value around 0
    float n = hash(cell + grain_seed) + hash(cell * 1.7 + grain_seed * 3.1) - 1.0;
    // Film grain shows most in the midtones and least in deep shadow and full highlights
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float response = 4.0 * luma * (1.0 - luma);
    return c + n * grain_strength * (0.25 + 0.75 * response);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 diff = texture(diffuseRect, tc);
    vec3 c = diff.rgb;

    if (ca_pixels > 0.0)
    {
        c = chromaticAberration(tc, c);
    }
    if (flare_strength > 0.0)
    {
        if (flare_hq != 0)
        {
            // Added as light, then eased toward white, so a strong flare over a bright sky does not
            // clip into a flat disc. Where no flare reaches this pixel (no sun in view and no bright
            // light picked, the usual frame) decoding and encoding again gives back the clamped
            // colour, to float rounding, so the six pow are skipped there.
            vec3 f = fsLensFlareHQ(tc);
            if (any(greaterThan(f, vec3(0.0))))
            {
                c = fsToDisplay(clamp(fsToLinear(clamp(c, 0.0, 1.0)) + f / (1.0 + f), 0.0, 1.0));
            }
            else
            {
                c = clamp(c, 0.0, 1.0);
            }
        }
        else
        {
            c += lensFlare(tc);
        }
    }
    if (grain_strength > 0.0)
    {
        c = filmGrain(tc, c);
    }

    vec4 result = vec4(clamp(c, 0.0, 1.0), diff.a);
    if (vignette.x > 0.0)
    {
        if (vignette_quantize.x > 0.0)
        {
            result = floor(result * vignette_quantize.x + 0.5) / vignette_quantize.x;
        }
        // as exoVignetteF
        vec2 vt = vary_fragcoord - 0.5;
        float vignette_val = 1.0 - dot(vt, vt);
        result.rgb *= clamp(pow(mix(1.0, vignette_val * vignette_val * vignette_val * vignette_val * vignette.z, vignette.x), vignette.y), 0.0, 1.0);
        if (vignette_quantize.y > 0.0)
        {
            result = floor(result * vignette_quantize.y + 0.5) / vignette_quantize.y;
        }
    }
    frag_color = result;
}
