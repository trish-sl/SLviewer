/**
 * @file class1/deferred/fsShadowExtrasF.glsl
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

// Extra terms of the deferred sun shadow, multiplied into the sun channel of the light map by
// sunLightF / sunLightSSAOF (fsSunShadowExtras). Built into the two deferred sun programs as one
// of their own files, so the program's permutation defines reach it:
//   FS_CLOUD_SHADOWS    the sky's cloud layer, projected along the light onto each pixel
//   FS_CONTACT_SHADOWS  a short depth buffer ray march towards the light (contact shadows)
// Neither runs in reflection probe captures (cube_snapshot).

uniform int cube_snapshot;
uniform vec3 sun_dir;   // view space
uniform vec3 moon_dir;  // view space
uniform int sun_up_factor;

// interleaved gradient noise (Jimenez 2014)
float fsExtrasNoise(vec2 frag)
{
    return fract(52.9829189 * fract(0.06711056 * frag.x + 0.00583715 * frag.y));
}

#if defined(FS_CLOUD_SHADOWS)
// The cloud layer of cloudsF.glsl, anchored to the world instead of the camera's sky dome: the
// dome's planar uv of a direction d is about ((d.east, -d.north) / 2) / cloud_scale, so a layer at
// height H gives each metre on it 1 / (2 H cloud_scale) of uv. Same noise textures, scroll,
// scale, coverage (cloud_shadow), densities and variance as the sky.
uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;

uniform mat4 fs_cloud_view_to_world;  // inverse modelview: view space to agent space (x east, y north, z up)
uniform vec4 fs_cloud_map;            // xy: uv1 under the camera (wrapped), z: uv per metre, w: layer height above the camera (m)
uniform vec3 fs_cloud_pos_density1;   // cloud_pos_density1 with the scroll, as the sky gets it
uniform vec3 fs_cloud_pos_density2;   // cloud_pos_density2
uniform vec4 fs_cloud_params;         // x: cloud_scale, y: cloud_variance, z: cloud_shadow (coverage), w: blend_factor
uniform vec2 fs_cloud_light_offset;   // lightnorm.xz * 0.0125: the sky's self shadow offset
uniform vec2 fs_cloud_shadow;         // x: strength 0-1 (0: off), y: softness 0-1

// Explicit LOD: the uv comes from the world position, so implicit derivatives jump across
// depth edges and pick a wrong mip in a line along every silhouette.
vec4 fsCloudNoise(vec2 uv, float lod)
{
    // blend_factor is uniform: endpoint skies need only one lookup
    if (fs_cloud_params.w <= 0.0)
    {
        return textureLod(cloud_noise_texture, uv, lod);
    }
    if (fs_cloud_params.w >= 1.0)
    {
        return textureLod(cloud_noise_texture_next, uv, lod);
    }
    return mix(textureLod(cloud_noise_texture, uv, lod), textureLod(cloud_noise_texture_next, uv, lod), fs_cloud_params.w);
}

// fraction (0..1) of the light the cloud layer lets through to view space position pos
float fsCloudShadow(vec3 pos, vec3 light_dir)
{
    float strength = fs_cloud_shadow.x;
    float cloud_scale = fs_cloud_params.x;
    if (strength <= 0.0 || cloud_scale < 0.001)
    {
        return 1.0;
    }

    mat3 rot = mat3(fs_cloud_view_to_world);
    vec3 p = rot * pos;                           // metres from the camera
    vec3 l = rot * light_dir;
    // a light near the horizon would smear the layer across the land: fade the shadows out
    float horizon = smoothstep(0.05, 0.25, l.z);
    if (horizon <= 0.0)
    {
        return 1.0;
    }

    // along the light to the layer
    vec2 q = p.xy + l.xy * ((fs_cloud_map.w - p.z) / max(l.z, 0.05));

    float softness = clamp(fs_cloud_shadow.y, 0.0, 1.0);
    // Mip of the base (uv1) frequency: a pixel's footprint, taken as about 1 mrad of the
    // distance (smooth in depth, unlike derivatives), in noise texels, plus the softness bias.
    // The taps below add log2 of their own frequency to it.
    float texels_per_metre = fs_cloud_map.z * float(textureSize(cloud_noise_texture, 0).x);
    float lod = log2(max(length(p) * 0.001 * texels_per_metre, 1.0)) + softness * 3.0;

    // the uv sets of cloudsV.glsl (before the scroll)
    vec2 uv1 = fs_cloud_map.xy + vec2(q.x, -q.y) * fs_cloud_map.z;
    vec2 uv2 = uv1 + fs_cloud_light_offset;
    vec2 uv3 = uv1 * 16.0;
    vec2 uv4 = uv2 * 16.0;

    // the density of cloudsF.glsl
    float cloud_variance = fs_cloud_params.y;
    float cloudDensity = 2.0 * (fs_cloud_params.z - 0.25);

    vec2 disturbance  = vec2(fsCloudNoise(uv1 / 8.0, lod - 3.0).x, fsCloudNoise((uv3 + uv1) / 16.0, lod).x) * cloud_variance * (1.0 - cloud_scale * 0.25);
    vec2 disturbance2 = vec2(fsCloudNoise((uv1 + uv3) / 4.0, lod + 2.0).x, fsCloudNoise((uv4 + uv2) / 8.0, lod + 1.0).x) * cloud_variance * (1.0 - cloud_scale * 0.25);

    uv1 += fs_cloud_pos_density1.xy + (disturbance * 0.2);
    uv3 += fs_cloud_pos_density2.xy;

    float density_variance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 + disturbance2.x + disturbance2.y) * 4.0);
    cloudDensity *= 1.0 - (density_variance * density_variance);

    float alpha1 = (fsCloudNoise(uv1, lod).x - 0.5) + (fsCloudNoise(uv3, lod + 4.0).x - 0.5) * fs_cloud_pos_density2.z;
    // the sky's gain of 10 gives hard edged clouds; softness lowers it
    alpha1 = min(max(alpha1 + cloudDensity, 0.0) * mix(10.0, 3.0, softness) * fs_cloud_pos_density1.z, 1.0);
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = clamp(alpha1, 0.0, 1.0);

    return 1.0 - clamp(strength, 0.0, 1.0) * horizon * alpha1;
}
#endif // FS_CLOUD_SHADOWS

#if defined(FS_CONTACT_SHADOWS)
// Screen-space contact shadows: a short march through the depth buffer from the pixel towards the
// light. Catches the small shadows the cascades are too coarse for (feet on the ground, folds,
// small props); multiplied into the cascaded shadow, so it can only darken.
uniform mat4 projection_matrix;
uniform vec2 screen_res;
// x: ray length (m), y: strength 0-1 (0: off), z: noise offset 0-1 (changes every frame while
// frames are blended: TAA, FSRenderPhotoShadows; -1 otherwise: no noise), w: most steps along the ray
uniform vec4 fs_contact_shadow;
// Non-zero in the pass that applies the contact term. The light map's sun channel is blurred
// after the sun pass (a dozen pixels each way), which spread a contact shadow a few pixels wide
// so thin it disappeared; so where the blur runs, its second pass applies the term to the
// blurred result (blurLightF), and the sun pass does it only where the blur is off.
uniform int fs_contact_here;
// FSRenderPhotoShadows: added to the light direction (view space) so the ray leaves for this frame's
// point of the sun's disc, as the shadow maps do; 0 otherwise
uniform vec3 fs_contact_light_jitter;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);

#define FS_CONTACT_MIN_STEPS  4      // steps along the ray: one per pixel it covers on screen,
#define FS_CONTACT_MAX_STEPS  48     // within these bounds (and fs_contact_shadow.w)
#define FS_CONTACT_FADE_NEAR  24.0   // metres from the camera: full strength up to here
#define FS_CONTACT_FADE_FAR   48.0   // and none from here on
#define FS_CONTACT_EDGE       0.04   // screen fraction at the edges over which hits fade out
#define FS_CONTACT_THICKNESS  0.10   // metres: how deep a surface in the depth buffer is taken to be,
#define FS_CONTACT_THICKNESS_PER_M 0.005 // plus this much per metre from the camera

// View-space position and raw depth of the depth buffer at uv, false for sky
bool fsContactNeighbour(vec2 uv, out vec3 p, out float d)
{
    d = getDepth(uv);
    if (isFarDepth(d))
    {
        p = vec3(0.0);
        return false;
    }
    p = getPositionWithDepth(uv, d).xyz;
    return true;
}

// How badly the side whose neighbours lie one and two pixels out (raw depths d1, d2) continues the
// pixel's own surface (raw depth dc): the error of extrapolating dc from them. A plane's raw depth
// is affine across the screen, so the side on this pixel's surface comes out near 0 and a side
// across an edge or a crease does not. A side with sky one pixel out never wins, one with sky two
// pixels out only over such a side.
float fsContactSideError(float dc, bool ok1, float d1, bool ok2, float d2)
{
    if (!ok1)
    {
        return 8.0;
    }
    if (!ok2)
    {
        return 4.0;
    }
    return abs(2.0 * d1 - d2 - dc);
}

// The surface normal the depth buffer itself describes, facing the camera. The march is tested
// against that same depth surface: with the G-buffer normal, a normal map or a smoothed avatar
// normal could aim the ray into the surface it leaves, which then shadowed itself.
// On each axis the neighbour is taken from the side whose two pixels continue this pixel's surface
// (fsContactSideError). The side with the smaller depth step, as before, took the other mesh at a
// crease or at an outline where that mesh was nearer in depth than this surface's own next pixel
// (a floor seen at a low angle steps many centimetres per pixel): the normal then pointed
// anywhere, the ray could start under the surface, and the pixel shadowed itself in a black speck
// or a one pixel line along the edge. A normal that still points into the surface the G-buffer
// describes is not used either.
vec3 fsContactDepthNormal(vec2 tc, vec3 pos, vec3 fallback)
{
    vec2 px = 1.0 / screen_res;
    float dc = getDepth(tc);
    vec3 pr, pl, pu, pd, pr2, pl2, pu2, pd2;
    float zr, zl, zu, zd, zr2, zl2, zu2, zd2;
    bool r = fsContactNeighbour(tc + vec2(px.x, 0.0), pr, zr);
    bool l = fsContactNeighbour(tc - vec2(px.x, 0.0), pl, zl);
    bool u = fsContactNeighbour(tc + vec2(0.0, px.y), pu, zu);
    bool d = fsContactNeighbour(tc - vec2(0.0, px.y), pd, zd);
    if (!(r || l) || !(u || d))
    {
        return fallback;
    }
    bool r2 = fsContactNeighbour(tc + vec2(2.0 * px.x, 0.0), pr2, zr2);
    bool l2 = fsContactNeighbour(tc - vec2(2.0 * px.x, 0.0), pl2, zl2);
    bool u2 = fsContactNeighbour(tc + vec2(0.0, 2.0 * px.y), pu2, zu2);
    bool d2 = fsContactNeighbour(tc - vec2(0.0, 2.0 * px.y), pd2, zd2);
    float er = fsContactSideError(dc, r, zr, r2, zr2);
    float el = fsContactSideError(dc, l, zl, l2, zl2);
    float eu = fsContactSideError(dc, u, zu, u2, zu2);
    float ed = fsContactSideError(dc, d, zd, d2, zd2);

    vec3 dr = pr - pos;
    vec3 dl = pos - pl;
    vec3 du = pu - pos;
    vec3 dd = pos - pd;
    // ties (both sides short of a second pixel) go to the smaller depth step
    bool use_r = er < el || (er == el && r && abs(dr.z) <= abs(dl.z));
    bool use_u = eu < ed || (eu == ed && u && abs(du.z) <= abs(dd.z));
    vec3 dx = use_r ? dr : dl;
    vec3 dy = use_u ? du : dd;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-10)
    {
        return fallback;
    }
    n /= len;
    n = dot(n, -pos) < 0.0 ? -n : n;
    return dot(n, fallback) > 0.0 ? n : fallback;
}

// How the march works, and what it fixed (on a depth buffer test scene, the old march found about
// half of the pixels a cup, a chair leg and a foot shadow within a metre, and missed most of the
// first few centimetres around them; this one finds nearly all of those):
//  - The ray starts a pixel's width off the surface along the depth buffer's own normal. That is
//    all the neighbouring texels of the same surface need to stay behind the ray, so a hit needs
//    no margin beyond depth precision. The old start, 1 to 8 cm off along the G-buffer normal,
//    jumped over the contact itself, and its hit margin of a centimetre and more missed a ray
//    passing through a thin chair leg.
//  - Steps get longer along the ray (t grows as s^1.5) and start right at the surface, so the first
//    centimetres are walked about a pixel at a time; the old even steps began one whole step out.
//  - A surface counts as an occluder when it is up to 10 cm (plus 5 mm per metre) in front of the
//    ray, about the depth of a limb or a chair leg; deeper ones drew halos around objects whose
//    silhouette the ray passed behind. Its deeper part (from 60 %) counts less and less, so the
//    shadow fades out where the test stops finding the occluder instead of ending in a line.
//  - Hits fade out from a fifth of the ray to its end, so a shadow pales away from its contact
//    and the end of the ray is no hard edge.
float fsContactShadow(vec3 pos, vec3 norm, vec3 light_dir)
{
    float len = fs_contact_shadow.x;
    float dist = -pos.z;
    float strength = clamp(fs_contact_shadow.y, 0.0, 1.0) * (1.0 - smoothstep(FS_CONTACT_FADE_NEAR, FS_CONTACT_FADE_FAR, dist));
    if (strength <= 0.0 || len <= 0.0 || dist <= 0.0 || dot(norm, light_dir) <= 0.0)
    {
        return 1.0; // far away, off, or facing away from the light (already dark)
    }

    vec4 clip_p = projection_matrix * vec4(pos, 1.0);
    if (clip_p.w <= 0.0)
    {
        return 1.0;
    }
    vec2 tc = clip_p.xy / clip_p.w * 0.5 + 0.5;
    vec3 n = fsContactDepthNormal(tc, pos, norm);
    float nl = dot(n, light_dir);
    if (nl <= 0.0)
    {
        return 1.0; // the surface as drawn faces away: the shadow map's terminator is there
    }
    strength *= smoothstep(0.0, 0.1, nl); // no hard edge where a rounded surface turns away

    // Width of a pixel at this distance (m): ndc y spans 2 over screen_res.y pixels
    float px_m = 2.0 * dist / (max(projection_matrix[1][1], 1e-4) * max(screen_res.y, 1.0));
    vec3 origin = pos + n * (px_m + 0.002);
    float thickness = FS_CONTACT_THICKNESS + FS_CONTACT_THICKNESS_PER_M * dist;
    float min_delta = 0.0015 + 0.0002 * dist; // depth precision

    // As many steps as the ray covers pixels, so a ray close to the camera doesn't step over
    // thin occluders and a distant one doesn't sample the same pixel over and over
    vec4 clip0 = projection_matrix * vec4(origin, 1.0);
    vec4 clip1 = projection_matrix * vec4(origin + light_dir * len, 1.0);
    float px = 0.0;
    if (clip0.w > 0.0 && clip1.w > 0.0)
    {
        px = length((clip1.xy / clip1.w - clip0.xy / clip0.w) * 0.5 * screen_res);
    }
    float max_steps = clamp(fs_contact_shadow.w, float(FS_CONTACT_MIN_STEPS), float(FS_CONTACT_MAX_STEPS));
    int steps = int(clamp(ceil(px), float(FS_CONTACT_MIN_STEPS), max_steps));
    // Jittered while frames are blended (TAA, the photo shadows): the banding becomes noise they
    // average away. Nothing else removes it: the light map blur runs before this term (see
    // fs_contact_here), so a still per-pixel jitter reached the screen as a stipple over every
    // contact shadow and along the outline of anything standing on a floor. Without a blend
    // (z < 0) every pixel steps the same way, so neighbours agree.
    float jitter = fs_contact_shadow.z < 0.0 ? 0.5 : fract(fsExtrasNoise(gl_FragCoord.xy) + fs_contact_shadow.z);
    float min_t = 0.5 * px_m;

    // Soft, not hit or miss. A hit used to be all or nothing at full strength wherever along the
    // ray it was, so the term drew a hard edged shape that ended exactly where the depth buffer
    // test stopped finding the occluder: along the outline of a leg on a floor (rays passing
    // behind it), and in stair steps where a coarse step jumped over the front few centimetres
    // that count as the occluder. Now each sample occludes by how well it fits (fading in just
    // past depth precision, out over the deeper part of the thickness), a hit far along the ray
    // is lighter (a real shadow widens and pales away from its contact; the cascades carry the
    // rest), and a ray that crosses a surface between two steps counts as a hit.
    float occ = 0.0;
    bool prev_valid = false;
    float prev_delta = 0.0;
    float prev_surf_z = 0.0;
    float prev_ray_z = 0.0;

    for (int i = 0; i < FS_CONTACT_MAX_STEPS; ++i)
    {
        if (i >= steps)
        {
            break;
        }
        float s = (float(i) + jitter) / float(steps);
        float t = max(len * s * sqrt(s), min_t);
        vec3 p = origin + light_dir * t;
        vec4 clip = projection_matrix * vec4(p, 1.0);
        if (clip.w <= 0.0)
        {
            break; // behind the camera
        }
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        {
            break; // left the screen
        }
        // full up to a fifth of the ray, then a smooth fall to none at its end; it only falls
        // along the ray, so once the shadow reaches it no later sample can add any more
        float fade = 1.0 - smoothstep(0.2, 1.0, t / len);
        if (occ >= fade)
        {
            break;
        }
        float d = getDepth(uv);
        if (isFarDepth(d))
        {
            prev_valid = false;
            continue;
        }
        // > 0: the surface the depth buffer has there is in front of the ray
        float surf_z = getPositionWithDepth(uv, d).z;
        float delta = surf_z - p.z;
        float w = smoothstep(min_delta, 2.0 * min_delta, delta) * (1.0 - smoothstep(0.6 * thickness, thickness, delta));
        // Stepped over: the ray was just in front of the surface at the last sample (by less than
        // the thickness) and is now deeper behind it than the thickness, and it got there by going
        // deeper itself: the surface moved in depth no more than the ray went away from the camera,
        // so it is the same surface the ray went through. With half the thickness of slack, and the
        // ray in front by any amount, a sample that went from a floor onto the outline of a mesh a
        // few centimetres nearer, where the two meet, counted as a full hit: black specks and short
        // lines along mesh edges. A ray coming toward the camera never steps behind a surface.
        float ray_deeper = prev_ray_z - p.z;
        if (prev_valid && prev_delta <= min_delta && prev_delta > -thickness && delta >= thickness &&
            abs(surf_z - prev_surf_z) <= ray_deeper)
        {
            w = 1.0;
        }
        vec2 edge = min(uv, vec2(1.0) - uv);
        w *= fade * smoothstep(0.0, FS_CONTACT_EDGE, min(edge.x, edge.y));
        occ = max(occ, w);
        prev_valid = true;
        prev_delta = delta;
        prev_surf_z = surf_z;
        prev_ray_z = p.z;
    }
    return 1.0 - strength * occ;
}

// the contact term for the sun channel of the light map, where this pass is the one to apply it
float fsContactShadowTerm(float shadow, vec3 pos, vec3 norm)
{
    if (fs_contact_here == 0 || cube_snapshot != 0 || shadow <= 0.0)
    {
        return shadow;
    }
    vec3 light_dir = normalize(normalize((sun_up_factor == 1) ? sun_dir : moon_dir) + fs_contact_light_jitter);
    return shadow * fsContactShadow(pos, norm, light_dir);
}
#endif // FS_CONTACT_SHADOWS

// sun shadow (light map red channel) of the pixel at view space position pos with normal norm
float fsSunShadowExtras(float shadow, vec3 pos, vec3 norm)
{
    if (cube_snapshot != 0)
    {
        return shadow; // probes keep the plain shadow: too rare for moving clouds, too coarse for contact
    }
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
#if defined(FS_CONTACT_SHADOWS)
    shadow = fsContactShadowTerm(shadow, pos, norm);
#endif
#if defined(FS_CLOUD_SHADOWS)
    if (shadow > 0.0)
    {
        shadow *= fsCloudShadow(pos, light_dir);
    }
#endif
    return shadow;
}
