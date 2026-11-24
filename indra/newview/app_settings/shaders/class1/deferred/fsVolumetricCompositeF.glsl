// Sun rays (or underwater rays) and volumetric projector light, both filtered up to full size in one pass
// (FSRenderMergePostPasses)

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending over the HDR scene, in place of fsSunRaysCompositeF followed by
// fsSpotVolumetricCompositeF: the same joint bilateral upsample of each map, with the same math, and
// their sum added once. The screen's depth is read and turned into a distance once for both.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // the rays, reduced resolution: r scattering, g distance marched to
uniform sampler2D specularRect; // the projectors, reduced resolution: rgb scattered light, a distance marched to
uniform vec3 ray_color;         // linear light colour times strength
uniform float vol_strength;

// Distance stored for the sky, as in fsSunRaysF and fsSpotVolumetricF
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

#ifdef FS_BICUBIC
// FSRenderBicubicUpsample: the 4x4 texels around the point, weighted by a cubic B-spline instead of
// the 3x3 Gaussian, whose window jumped by a texel as the reduced grid moved under the pixels.
// The weight is separable (fsBSplineWeight(d) = fsBSpline(d.x) * fsBSpline(d.y)), so each axis's
// four weights are taken once, from the same clamped texel coordinates the taps use.
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

    // The rays, as fsSunRaysCompositeF
    vec3 rays;
    {
        ivec2 size = textureSize(diffuseRect, 0);
        vec2 low_pos = tc * vec2(size) - 0.5;   // in reduced resolution texel centres
#ifdef FS_BICUBIC
        ivec2 base = ivec2(floor(low_pos));     // taps -1 to +2
#else
        ivec2 base = ivec2(floor(low_pos + 0.5));
#endif

        float total = 0.0;
        float weight_sum = 0.0;
        float best_value = 0.0;
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
                vec2 tap = texelFetch(diffuseRect, p, 0).rg;
                float rel = abs(tap.g - center_dist) / max(center_dist, 0.1);
#ifdef FS_BICUBIC
                float w = (wx[x + 1] * wy[y + 1]) * exp(-10.0 * rel);
#else
                vec2 d = vec2(p) - low_pos;
                float w = exp(-0.5 * dot(d, d) - 10.0 * rel);
#endif
                total += tap.r * w;
                weight_sum += w;
                if (rel < best_rel)
                {
                    best_rel = rel;
                    best_value = tap.r;
                }
            }
        }
        float scattered = weight_sum > 1.0e-20 ? total / weight_sum : best_value;
        rays = ray_color * max(scattered, 0.0);
    }

    // The projectors, as fsSpotVolumetricCompositeF
    vec3 spots;
    {
        ivec2 size = textureSize(specularRect, 0);
        vec2 low_pos = tc * vec2(size) - 0.5;
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
                vec4 tap = texelFetch(specularRect, p, 0);
                float rel = abs(tap.a - center_dist) / max(center_dist, 0.1);
#ifdef FS_BICUBIC
                float w = (wx[x + 1] * wy[y + 1]) * exp(-10.0 * rel);
#else
                vec2 d = vec2(p) - low_pos;
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
        vec3 scattered = weight_sum > 1.0e-20 ? total / weight_sum : best_value;
        spots = max(scattered, vec3(0.0)) * vol_strength;
    }

    frag_color = vec4(rays + spots, 0.0);
}
