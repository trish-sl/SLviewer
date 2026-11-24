/**
 * @file exposureF.glsl
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D emissiveRect;
#ifdef USE_LAST_EXPOSURE
uniform sampler2D exposureMap;
#endif

uniform float dt;
uniform vec2 noiseVec;

uniform vec4 dynamic_exposure_params;
uniform vec4 dynamic_exposure_params2;
// x speed multiplier adapting to a brighter scene, y to a darker one, z centre weight (0 = the
// whole metered area), w 1 when the luminance map holds log2 luminance (FSRenderExposureLogMetering)
uniform vec4 fs_exposure_params;

float lum(vec3 col)
{
    vec3 l = vec3(0.2126, 0.7152, 0.0722);
    return dot(l, col);
}

void main()
{
    vec2 tc = vec2(0.5,0.5);

    float L = textureLod(emissiveRect, tc, 8).r;
    if (fs_exposure_params.z > 0.0)
    {
        // Centre weighted metering: the middle of the 256 square luminance map, from the
        // centre sample of the 8x8 and 4x4 levels (the middle quarter and half of it)
        float centre = 0.5 * (textureLod(emissiveRect, tc, 5).r + textureLod(emissiveRect, tc, 6).r);
        L = mix(L, centre, clamp(fs_exposure_params.z, 0.0, 1.0));
    }
    // <FS:Trish> Log metering: the map's mips average log2 luminance, so this is the geometric
    // mean, which one bright light or the sun disc can't drag up the way it does the plain mean
    if (fs_exposure_params.w > 0.0)
    {
        L = exp2(L);
    }
    // A NaN in the scene reaches the last mip; clamp() of a NaN is undefined, and the exposure
    // would keep it for good through the history. Meter it as dark for one frame instead.
    bool fs_bad_L = isnan(L);
    L = fs_bad_L ? 0.0 : L;
    // </FS:Trish>
    float max_L = dynamic_exposure_params.x;
    L = clamp(L, 0.0, max_L);
    L /= max_L;
    L = pow(L, 2.0);
    float s = mix(dynamic_exposure_params.z, dynamic_exposure_params.y, L);
#ifdef USE_LAST_EXPOSURE
    float prev = texture(exposureMap, vec2(0.5,0.5)).r;
    // <FS:Trish> A history that went bad (NaN, infinity) restarts from this frame, and a bad meter
    // reading keeps the last exposure. An exposure of 0 is a real one (a sky whose HDR offset is
    // its minimum) and eases like any other: restarting on it snapped the exposure every frame.
    // (A new map starts at 1, LLPipeline::createGLBuffers clears it so.)
    if (isnan(prev) || isinf(prev) || prev < 0.0)
    {
        prev = s;
    }
    if (fs_bad_L)
    {
        s = prev;
    }
    // </FS:Trish>

    float speed = -log(dynamic_exposure_params.w) / dynamic_exposure_params2.w;
    // A brighter scene lowers the exposure scale, a darker one raises it
    speed *= s < prev ? fs_exposure_params.x : fs_exposure_params.y;
    // <FS:Trish> Blend in stops (log2) rather than in the multiplier: the eye adapts by ratios, so
    // a step from 4x to 1x and one from 1x to 4x take the same time. The plain blend made
    // brightening and darkening run at different apparent speeds.
    //s = mix(prev, s, 1 - exp(-speed * dt));
    float fs_k = 1.0 - exp(-speed * dt);
    if (min(prev, s) > 1e-4)
    {
        s = exp2(mix(log2(prev), log2(s), fs_k));
    }
    else
    {
        s = mix(prev, s, fs_k); // a range down to 0 (sky settings) has no log
    }
    // </FS:Trish>
#endif

    frag_color = max(vec4(s, s, s, dt), vec4(0.0));
}

