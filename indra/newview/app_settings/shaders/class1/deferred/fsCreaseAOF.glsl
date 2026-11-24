/**
 * @file class1/deferred/fsCreaseAOF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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
 * $/LicenseInfo$
 */

// Crease occlusion (FSRenderCreaseAO): a second, short range ambient occlusion term, at full
// resolution, that the sun pass (sunLightSSAOF) multiplies into the light map's occlusion channel
// after GTAO or the classic SSAO.
//
// Those look up to a metre around each pixel (and GTAO at half resolution where set), so the few
// centimetres of an inside corner, the gap under a door, a fold or the seam between two prims get
// little weight among all the open space further out. This term looks only FSRenderCreaseAORadius
// around the pixel (a few centimetres, capped at 24 pixels on screen), so what it finds is the
// crease itself. It is the horizon search of GTAO (Jimenez 2016): two slices through the view
// vector, the highest horizon on each side within the radius, and the cosine-weighted share of
// the hemisphere left open between them. The slices are turned by a per-pixel noise the light map
// blur that follows removes; the blur keeps to each surface's plane, so it doesn't smear the crease.

uniform vec2 screen_res;
uniform mat4 inv_proj;
uniform vec4 fs_crease_ao;   // x: radius (m), y: strength 0-1, z: largest radius in pixels, w: noise offset 0-1

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec3 fsPositionWithDepthPersp(vec2 pos_screen, float depth); // deferredUtil.glsl: the camera's perspective inverse, seven terms

#define FS_CREASE_SLICES 2
#define FS_CREASE_STEPS  4
#define FS_CREASE_PI      3.14159265
#define FS_CREASE_HALF_PI 1.57079633
#define FS_CREASE_ANGLE_BIAS 0.15 // radians

float fsCreaseAcos(float x)
{
    float ax = abs(x);
    float r = (-0.156583 * ax + FS_CREASE_HALF_PI) * sqrt(max(1.0 - ax, 0.0));
    return x >= 0.0 ? r : FS_CREASE_PI - r;
}

bool fsCreaseNeighbour(vec2 tc, out vec3 p)
{
    float d = getDepth(tc);
    if (isFarDepth(d))
    {
        p = vec3(0.0);
        return false;
    }
    //p = getPositionWithDepth(tc, d).xyz;
    p = fsPositionWithDepthPersp(tc, d);
    return true;
}

// The surface normal the depth buffer describes (as fsGTAODepthNormal): the occlusion is measured on
// the depth surface, so a normal map's bumps don't occlude a flat wall
vec3 fsCreaseDepthNormal(vec2 tc, vec3 pos, vec3 fallback)
{
    vec2 px = 1.0 / screen_res;
    vec3 pr, pl, pu, pd;
    bool r = fsCreaseNeighbour(tc + vec2(px.x, 0.0), pr);
    bool l = fsCreaseNeighbour(tc - vec2(px.x, 0.0), pl);
    bool u = fsCreaseNeighbour(tc + vec2(0.0, px.y), pu);
    bool d = fsCreaseNeighbour(tc - vec2(0.0, px.y), pd);
    if (!(r || l) || !(u || d))
    {
        return fallback;
    }
    vec3 dr = pr - pos;
    vec3 dl = pos - pl;
    vec3 du = pu - pos;
    vec3 dd = pos - pd;
    vec3 dx = (r && (!l || abs(dr.z) <= abs(dl.z))) ? dr : dl;
    vec3 dy = (u && (!d || abs(du.z) <= abs(dd.z))) ? du : dd;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-10)
    {
        return fallback;
    }
    n /= len;
    return dot(n, -pos) < 0.0 ? -n : n;
}

// One sample folded into a slice side's horizon cosine. Off-screen and sky samples count as open;
// a sample beyond the radius fades out over its last half, so the occlusion has no hard edge.
float fsCreaseHorizon(vec2 tc, vec3 pos, vec3 v, float radius, float low_cos, float horizon_cos)
{
    vec2 ctc = clamp(tc, vec2(0.0), vec2(1.0));
    float d = getDepth(ctc);
    bool valid = ctc == tc && !isFarDepth(d);
    // a sky depth unprojects to infinity (a division by zero under reverse-Z): use a finite one
    //vec3 delta = getPositionWithDepth(ctc, valid ? d : 0.5).xyz - pos;
    vec3 delta = fsPositionWithDepthPersp(ctc, valid ? d : 0.5) - pos;
    float dist = max(length(delta), 1e-5);
    float raw_cos = dot(delta, v) / dist;
    float weight = valid ? clamp(2.0 - 2.0 * dist / radius, 0.0, 1.0) : 0.0;
    float sample_cos = mix(low_cos, raw_cos, weight);
    if (sample_cos > horizon_cos)
    {
        return sample_cos;
    }
    // a sample below the running horizon pulls it down a little (GTAO's thickness heuristic):
    // the depth buffer has no back faces, so a thin rail would otherwise count as a wall. Only
    // one that lies below it in the depth buffer too: a surface standing in front but out of
    // range (an avatar before the wall) would wear the crease away around its outline.
    return (valid && raw_cos > horizon_cos) ? horizon_cos : max(mix(horizon_cos, sample_cos, 0.1), low_cos);
}

// Visibility (1 open, 0 closed) of the crease around view space position pos at screen position
// tc, on the strength curve; norm is the G-buffer normal, used where the depth buffer has none.
float fsCreaseAO(vec3 pos, vec3 norm, vec2 tc)
{
    float strength = clamp(fs_crease_ao.y, 0.0, 1.0);
    float view_depth = -pos.z;
    if (strength <= 0.0 || fs_crease_ao.x <= 0.0 || view_depth <= 1e-4)
    {
        return 1.0;
    }

    // the radius on screen, capped: beyond the cap, a nearby surface keeps its occlusion in
    // pixels and so gets a smaller radius in metres
    vec2 radius_tc = fs_crease_ao.x * 0.5 / (view_depth * vec2(inv_proj[0][0], inv_proj[1][1]));
    float radius_px = radius_tc.y * screen_res.y;
    float clamp_scale = min(max(fs_crease_ao.z, 2.0) / max(radius_px, 1e-4), 1.0);
    radius_tc *= clamp_scale;
    radius_px *= clamp_scale;
    if (radius_px < 1.5)
    {
        return 1.0; // the crease is smaller than a pixel or two here
    }
    float radius = fs_crease_ao.x * clamp_scale;

    vec3 v = normalize(-pos);
    vec3 n = fsCreaseDepthNormal(tc, pos, normalize(norm));

    // interleaved gradient noise turns the slices (moving on every frame while frames are blended)
    vec2 frag_px = gl_FragCoord.xy;
    float slice_noise = fract(fract(52.9829189 * fract(dot(frag_px, vec2(0.06711056, 0.00583715)))) + fs_crease_ao.w);
    float step_noise = 0.25 * float((int(frag_px.y) + 3 * int(frag_px.x)) & 3) + 0.125;
    float min_s = 1.0 / radius_px; // not the pixel itself

    float visibility = 0.0;
    for (int slice = 0; slice < FS_CREASE_SLICES; ++slice)
    {
        float phi = (float(slice) + slice_noise) * (FS_CREASE_PI / float(FS_CREASE_SLICES));
        vec2 omega = vec2(cos(phi), sin(phi));

        vec3 dir = vec3(omega, 0.0);
        vec3 ortho_dir = dir - dot(dir, v) * v;
        vec3 axis = normalize(cross(ortho_dir, v));
        vec3 proj_n = n - axis * dot(n, axis);
        float proj_n_len = length(proj_n);

        float sign_n = sign(dot(ortho_dir, proj_n));
        float cos_n = clamp(dot(proj_n, v) / max(proj_n_len, 1e-4), 0.0, 1.0);
        float n_ang = sign_n * fsCreaseAcos(cos_n);
        // the sine of the normal's angle, as the arcs below take it (sin of the approximate acos,
        // not sqrt(1 - cos_n^2)); cos(a + pi/2) = -sin(a) and cos(a - pi/2) = sin(a)
        float sin_n = sin(n_ang);

        //float low_cos0 = cos(n_ang + FS_CREASE_HALF_PI);
        //float low_cos1 = cos(n_ang - FS_CREASE_HALF_PI);
        float low_cos0 = -sin_n;
        float low_cos1 = sin_n;
        float horizon_cos0 = low_cos0;
        float horizon_cos1 = low_cos1;

        for (int i = 0; i < FS_CREASE_STEPS; ++i)
        {
            float s = (float(i) + step_noise) / float(FS_CREASE_STEPS);
            s = s * s + min_s;
            vec2 offset = omega * radius_tc * s;
            horizon_cos0 = fsCreaseHorizon(tc + offset, pos, v, radius, low_cos0, horizon_cos0);
            horizon_cos1 = fsCreaseHorizon(tc - offset, pos, v, radius, low_cos1, horizon_cos1);
        }

        float h0 = -fsCreaseAcos(horizon_cos1);
        float h1 =  fsCreaseAcos(horizon_cos0);
        h0 = n_ang + clamp(h0 - n_ang, -FS_CREASE_HALF_PI, FS_CREASE_HALF_PI);
        h1 = n_ang + clamp(h1 - n_ang, -FS_CREASE_HALF_PI, FS_CREASE_HALF_PI);
        // horizons barely above the tangent plane are the edges between a rounded mesh's triangles
        // (see FS_GTAO_ANGLE_BIAS in fsGTAOF.glsl), not a crease
        h0 = max(h0 - FS_CREASE_ANGLE_BIAS, n_ang - FS_CREASE_HALF_PI);
        h1 = min(h1 + FS_CREASE_ANGLE_BIAS, n_ang + FS_CREASE_HALF_PI);

        //float sin_n = sin(n_ang);
        float arc0 = cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - n_ang);
        float arc1 = cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n_ang);
        visibility += proj_n_len * 0.25 * (arc0 + arc1);
    }
    visibility = clamp(visibility / float(FS_CREASE_SLICES), 0.0, 1.0);

    return mix(1.0, visibility, strength);
}
