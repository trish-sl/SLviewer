/**
 * @file fsPointShadowMapF.glsl
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

// Shadow maps of the local point lights (FSRenderLocalLightShadows, fspointlightshadows.cpp).
// Each shadowed light (a slot) has a cube of six depth maps, the six 90 degree views from the
// light along the agent axes, rendered with the shadow casters of the sun and projector passes.
// They share one depth texture: slot s is row s of the atlas, face f column f. Each face is drawn
// a little wider than 90 degrees, so a guard band of a few texels around it holds what lies just
// over its edge and the filter below never reads outside the face.
// Linked into the local light programs with the FS_POINT_SHADOW_MAP permutation; the pipeline
// binds the atlas to fs_lls_map (compare mode on) and sets the uniforms with the program.

/*[EXTRA_CODE_HERE]*/

uniform sampler2DShadow fs_lls_map;
uniform vec4 fs_lls_light[FS_LLS_MAX];  // per slot: agent space position the map was drawn from, w: strength (0: none)
uniform vec4 fs_lls_depth[FS_LLS_MAX];  // per slot: depth terms A and B (window depth 0.5 * (A - B / m) + 0.5
                                        // at distance m along a face's axis), near and far clip (m)
uniform vec4 fs_lls_atlas;              // x: a face's width in the atlas (1/6), y: a slot's height (1/rows),
                                        // z: the faces' projection scale (1 / tan of the half angle), w: texels per face
uniform int fs_lls_debug;               // FSRenderLocalLightShadowDebug: 1 to 4 tint each mapped light by its slot,
                                        // 5 and 6 show the lookup's inputs (fsPointShadowMap)
// The view LLRender gives this draw, the one its surface positions are in. The way from eye space
// to the faces' (agent) axes and the light's eye space centre are both taken from it, so nothing
// the C++ uploads can disagree with the positions. (A separately uploaded rotation was copied out
// of a glm::mat3 as nine packed floats, but GLM_FORCE_DEFAULT_ALIGNED_GENTYPES pads each column
// to four, so two of its axes were shifted and the shadows turned with the camera.)
uniform mat4 modelview_matrix;

// The light's eye space centre, from its agent space one
vec3 fsPointShadowLightEye(vec3 center)
{
    return (modelview_matrix * vec4(center, 1.0)).xyz;
}

// Light to surface along the agent axes: pos and the light's centre in eye space, rotated back
vec3 fsPointShadowToAgent(vec3 v)
{
    return transpose(mat3(modelview_matrix)) * v;
}

// The surface's position along the agent axes, once per pixel. The draw's view is a rotation R
// and a translation t (eye = R agent + t), so R^T (pos - t) is the surface in agent space, and
// light to surface is then its difference from the light's agent space centre (fs_lls_light): a
// light loop spends a subtraction per shadowed light where it spent the light's eye position, a
// subtraction and a rotation, and the normal's rotation once instead of per light.
vec3 fsPointShadowPosAgent(vec3 pos)
{
    return transpose(mat3(modelview_matrix)) * (pos - modelview_matrix[3].xyz);
}

// A direction (the surface normal) along the agent axes, once per pixel
vec3 fsPointShadowDirAgent(vec3 v)
{
    return fsPointShadowToAgent(v);
}

// Offsets in texels of the face at the surface's distance, so they grow with it as the texels do:
// along the surface normal (more at grazing light), then towards the light along the ray
#define FS_LLS_NORMAL_OFFSET 0.75
#define FS_LLS_SLOPE_OFFSET  1.5
#define FS_LLS_DEPTH_BIAS    1.0

// 1: lit, down to 1 - strength: the slot's map has something between pos and the light.
// pos_agent, n_agent: the lit surface's position and normal along the agent axes
// (fsPointShadowPosAgent, fsPointShadowDirAgent).
// Filtered with a fixed 3x3 tent of hardware compares (each a bilinear 2x2), the same taps on
// every pixel: smooth edges without noise, nothing for TAA to blend.
float fsPointShadowFactor(int slot, vec3 pos_agent, vec3 n_agent)
{
    vec4 light = fs_lls_light[slot];
    if (light.w <= 0.0)
    {
        return 1.0;
    }
    vec4 dp = fs_lls_depth[slot];

    vec3 d = pos_agent - light.xyz; // light to surface, along the faces' axes
    float dist = length(d);
    vec3 a = abs(d);
    float m = max(a.x, max(a.y, a.z));
    if (m >= dp.w || dist <= 1e-4)
    {
        return 1.0; // past the far clip, where the light doesn't reach either
    }

    // one texel of a face at this distance, in metres
    float texel = 2.0 * m / (fs_lls_atlas.z * fs_lls_atlas.w);
    vec3 nw = n_agent;
    float nl = clamp(-dot(nw, d) / dist, 0.0, 1.0);
    d += nw * (texel * (FS_LLS_NORMAL_OFFSET + FS_LLS_SLOPE_OFFSET * sqrt(1.0 - nl * nl)));

    a = abs(d);
    m = max(a.x, max(a.y, a.z));
    if (m <= dp.z)
    {
        return 1.0; // nearer the light than the near clip: nothing in the map is in front
    }

    // The face looking along the major axis, and the position across it as that face's view has
    // it (right, up): the face axes of fspointlightshadows.cpp
    float face;
    vec2 st;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0.0 ? 0.0 : 1.0;
        st = vec2(d.x > 0.0 ? -d.y : d.y, d.z);
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0.0 ? 2.0 : 3.0;
        st = vec2(d.y > 0.0 ? d.x : -d.x, d.z);
    }
    else
    {
        face = d.z > 0.0 ? 4.0 : 5.0;
        st = vec2(d.z > 0.0 ? -d.x : d.x, d.y);
    }

    vec2 tile = st * (fs_lls_atlas.z / m) * 0.5 + 0.5;  // 0-1 across the face
    vec2 uv = (vec2(face, float(slot)) + tile) * fs_lls_atlas.xy;
    float ref = 0.5 * (dp.x - dp.y / max(m - texel * FS_LLS_DEPTH_BIAS, dp.z)) + 0.5;
    vec2 du = fs_lls_atlas.xy / fs_lls_atlas.w;         // one texel of the atlas

    float s = 4.0 * texture(fs_lls_map, vec3(uv, ref));
    s += 2.0 * (texture(fs_lls_map, vec3(uv + vec2(du.x, 0.0), ref)) +
                texture(fs_lls_map, vec3(uv - vec2(du.x, 0.0), ref)) +
                texture(fs_lls_map, vec3(uv + vec2(0.0, du.y), ref)) +
                texture(fs_lls_map, vec3(uv - vec2(0.0, du.y), ref)));
    s += texture(fs_lls_map, vec3(uv + du, ref)) +
         texture(fs_lls_map, vec3(uv - du, ref)) +
         texture(fs_lls_map, vec3(uv + vec2(du.x, -du.y), ref)) +
         texture(fs_lls_map, vec3(uv + vec2(-du.x, du.y), ref));
    s *= 1.0 / 16.0;

    return mix(1.0, s, clamp(light.w, 0.0, 1.0));
}

// FSRenderLocalLightShadowDebug: the colour of a slot, so that the light each map is applied to
// shows on screen. Red, green, blue, yellow, magenta, cyan, orange, white.
vec3 fsPointShadowSlotColor(int slot)
{
    if (slot == 0) return vec3(1.0, 0.1, 0.1);
    if (slot == 1) return vec3(0.1, 1.0, 0.1);
    if (slot == 2) return vec3(0.15, 0.3, 1.0);
    if (slot == 3) return vec3(1.0, 1.0, 0.1);
    if (slot == 4) return vec3(1.0, 0.1, 1.0);
    if (slot == 5) return vec3(0.1, 1.0, 1.0);
    if (slot == 6) return vec3(1.0, 0.5, 0.0);
    return vec3(1.0);
}

// What a light pass multiplies the light's contribution by: the shadow of slot `slot`'s map on
// the surface at pos_agent with normal n_agent (both along the agent axes, fsPointShadowPosAgent
// and fsPointShadowDirAgent, taken once per pixel by a pass with several lights), tinted by the
// slot's colour with FSRenderLocalLightShadowDebug. shading_light: the eye space centre the pass
// lights from.
// Debug 5: half metre bands along the agent axes, measured from the map's centre the way the
// lookup measures (the draw's view, fs_lls_light, pos): they stay fixed to the world as the
// camera moves when those inputs are right. Debug 6: green where the pass lights from the map's
// centre, red where it lights from elsewhere (more than 5 cm off).
vec3 fsPointShadowMapAgent(int slot, vec3 pos_agent, vec3 n_agent, vec3 shading_light)
{
    if (slot < 0 || slot >= FS_LLS_MAX)
    {
        return vec3(1.0);
    }
    if (fs_lls_debug >= 5 && fs_lls_light[slot].w > 0.0)
    {
        if (fs_lls_debug == 6)
        {
            vec3 light_eye = fsPointShadowLightEye(fs_lls_light[slot].xyz);
            return length(shading_light - light_eye) > 0.05 ? vec3(4.0, 0.0, 0.0) : vec3(0.0, 4.0, 0.0);
        }
        vec3 d = pos_agent - fs_lls_light[slot].xyz;
        return vec3(0.2) + 2.0 * step(vec3(0.5), fract(d * 2.0));
    }
    float s = fsPointShadowFactor(slot, pos_agent, n_agent);
    if (fs_lls_debug != 0 && fs_lls_light[slot].w > 0.0)
    {
        return s * fsPointShadowSlotColor(slot);
    }
    return vec3(s);
}

// The same from the surface's eye space position pos and normal n: one light per pass
vec3 fsPointShadowMap(int slot, vec3 pos, vec3 n, vec3 shading_light)
{
    return fsPointShadowMapAgent(slot, fsPointShadowPosAgent(pos), fsPointShadowDirAgent(n), shading_light);
}
