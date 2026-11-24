// Volumetric projector light: filter the reduced resolution scattering up to full size

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending over the HDR scene. The joint bilateral upsample of
// fsSunRaysCompositeF, on a colour: the 3x3 reduced resolution texels around this pixel, each
// weighted by how near it is and by how close the distance it was marched to (a) is to this
// pixel's own, so light cones don't bleed across silhouettes and the per pixel step offsets of
// the march average out.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // reduced resolution: rgb scattered light, a distance marched to
uniform float vol_strength;

// Distance stored for the sky, as in fsSpotVolumetricF
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

#ifdef FS_BICUBIC
// FSRenderBicubicUpsample: the 4x4 texels around the point, weighted by a cubic B-spline instead of
// the 3x3 Gaussian, whose window jumped by a texel as the reduced grid moved under the pixels
// The weight is separable (fsBSplineWeight(d) = fsBSpline(d.x) * fsBSpline(d.y)), so each axis's
// four weights are taken once, from the same clamped texel coordinates the taps use
float fsBSpline(float x); // fsBicubicF.glsl
#define FS_UP_LAST 2
#else
#define FS_UP_LAST 1
#endif

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    float center_dist = isFarDepth(depth) ? SKY_DIST : min(length(getPositionWithDepth(tc, depth).xyz), SKY_DIST);

    ivec2 size = textureSize(diffuseRect, 0);
    vec2 low_pos = tc * vec2(size) - 0.5;   // in reduced resolution texel centres
#ifdef FS_BICUBIC
    ivec2 base = ivec2(floor(low_pos));     // taps -1 to +2
#else
    ivec2 base = ivec2(floor(low_pos + 0.5));
#endif

    vec3 total = vec3(0.0);
    float weight_sum = 0.0;
    vec3 best_value = vec3(0.0);
    float best_rel = 1.0e20;
#ifdef FS_BICUBIC
    float wx[4];
    float wy[4];
    for (int k = 0; k < 4; ++k)
    {
        wx[k] = fsBSpline(float(clamp(base.x + k - 1, 0, size.x - 1)) - low_pos.x);
        wy[k] = fsBSpline(float(clamp(base.y + k - 1, 0, size.y - 1)) - low_pos.y);
    }
#endif
    for (int y = -1; y <= FS_UP_LAST; ++y)
    {
        for (int x = -1; x <= FS_UP_LAST; ++x)
        {
            ivec2 p = clamp(base + ivec2(x, y), ivec2(0), size - ivec2(1));
            vec4 tap = texelFetch(diffuseRect, p, 0);
            vec2 d = vec2(p) - low_pos;
            // Relative distance difference, so the same tolerance works near and far
            float rel = abs(tap.a - center_dist) / max(center_dist, 0.1);
#ifdef FS_BICUBIC
            //float w = fsBSplineWeight(d) * exp(-10.0 * rel);
            float w = (wx[x + 1] * wy[y + 1]) * exp(-10.0 * rel);
#else
            float w = exp(-0.5 * dot(d, d) - 10.0 * rel);
#endif
            total += tap.rgb * w;
            weight_sum += w;
            if (rel < best_rel)
            {
                best_rel = rel;
                best_value = tap.rgb;
            }
        }
    }

    // A thin full resolution detail that no reduced texel saw: take the closest match
    vec3 scattered = weight_sum > 1.0e-20 ? total / weight_sum : best_value;
    frag_color = vec4(max(scattered, vec3(0.0)) * vol_strength, 0.0);
}
