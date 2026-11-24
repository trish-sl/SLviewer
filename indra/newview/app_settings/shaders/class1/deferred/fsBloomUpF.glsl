// Bloom: spread a smaller level back up onto the next larger one

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending onto the larger level, which still holds its own downsampled
// image, so every level's blur ends up summed into the top one.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // smaller level
uniform vec2 src_texel;         // one texel of diffuseRect
uniform int fs_bloom_hq;        // FSRenderBloomHQFilter: a round 13 tap Gaussian instead of the ring below

void main()
{
    vec2 tc = vary_fragcoord.xy;

    if (fs_bloom_hq != 0)
    {
        // A round kernel: the centre, six taps on a hexagon and six more between and beyond them,
        // with Gaussian weights. A square 3x3 tent is the product of two 1D tents, whose outlines
        // are diamonds and squares; summed over every level, a very bright source such as the sun
        // then blooms with a squarish edge. The taps spread as far as the tent's did (a variance
        // of 4/3 texels squared per axis), so the bloom keeps its size.
        const float R1 = 1.71;
        const float R2 = 2.96;      // R1 * sqrt(3): the hexagon's next ring, turned 30 degrees
        const float W1 = 0.383;     // exp(-R1^2 / (2 * 1.524))
        const float W2 = 0.0565;    // exp(-R2^2 / (2 * 1.524))
        vec3 t = texture(diffuseRect, tc).rgb;
        vec3 ring1 = vec3(0.0);
        vec3 ring2 = vec3(0.0);
        // The directions as constants: every 60 degrees from 0, and every 60 from 30, rather than
        // twelve sines and cosines per pixel
        const vec2 fs_dir1[6] = vec2[6](vec2( 1.0,  0.0), vec2( 0.5,  0.8660254), vec2(-0.5,  0.8660254),
                                        vec2(-1.0,  0.0), vec2(-0.5, -0.8660254), vec2( 0.5, -0.8660254));
        const vec2 fs_dir2[6] = vec2[6](vec2( 0.8660254,  0.5), vec2( 0.0,  1.0), vec2(-0.8660254,  0.5),
                                        vec2(-0.8660254, -0.5), vec2( 0.0, -1.0), vec2( 0.8660254, -0.5));
        for (int i = 0; i < 6; ++i)
        {
            //float a = float(i) * 1.0471976; // 60 degrees
            //vec2 d1 = vec2(cos(a), sin(a));
            //vec2 d2 = vec2(cos(a + 0.5235988), sin(a + 0.5235988));
            vec2 d1 = fs_dir1[i];
            vec2 d2 = fs_dir2[i];
            ring1 += texture(diffuseRect, tc + d1 * R1 * src_texel).rgb;
            ring2 += texture(diffuseRect, tc + d2 * R2 * src_texel).rgb;
        }
        t = (t + ring1 * W1 + ring2 * W2) / (1.0 + 6.0 * W1 + 6.0 * W2);
        frag_color = vec4(t, 1.0);
        return;
    }

    // A ring: four edge taps two texels out, four diagonal taps one texel out at double weight
    vec3 c = texture(diffuseRect, tc + vec2(-2.0,  0.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 2.0,  0.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 0.0, -2.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 0.0,  2.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb * 2.0;

    frag_color = vec4(c / 12.0, 1.0);
}
