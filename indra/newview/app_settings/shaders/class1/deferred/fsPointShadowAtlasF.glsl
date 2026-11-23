/**
 * @file class1/deferred/fsPointShadowAtlasF.glsl
 * @brief The local light shadow map atlas on screen (FSRenderLocalLightShadowDebug 2 and up)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// The atlas of fspointlightshadows.cpp as it is: one row per slot (slot 0 at the bottom), the faces
// +X -X +Y -Y +Z -Z across. Each texel's depth as the distance from the light along the face's
// axis, near the light white falling to black at the far clip; dark blue where nothing was drawn,
// dark red rows for empty slots. Yellow lines between the faces and the slots.

out vec4 frag_color;

uniform sampler2D depthMap;     // the atlas, its compare mode off while drawn here
uniform vec4 fs_lls_depth[8];   // per slot: depth terms A and B, near and far clip (FSPointLightShadows::MAX_LIGHTS)
uniform vec4 fs_lls_info;       // x: faces across, y: rows

in vec2 vary_fragcoord;

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec2 cells = tc * fs_lls_info.xy;
    vec2 cell_pos = fract(cells);
    vec2 edge = fwidth(cells) * 1.5;
    if (any(lessThan(cell_pos, edge)) || any(greaterThan(cell_pos, vec2(1.0) - edge)))
    {
        frag_color = vec4(1.0, 0.85, 0.0, 1.0);
        return;
    }

    int row = int(clamp(floor(cells.y), 0.0, fs_lls_info.y - 1.0));
    vec4 dp = fs_lls_depth[row];
    if (dp.w <= dp.z || dp.z <= 0.0)
    {
        frag_color = vec4(0.25, 0.0, 0.0, 1.0);
        return;
    }

    float d = texture(depthMap, tc).r;
    if (d >= 1.0)
    {
        frag_color = vec4(0.0, 0.0, 0.2, 1.0);
        return;
    }
    float z = d * 2.0 - 1.0;
    float m = dp.y / max(dp.x - z, 1e-6);  // window depth 0.5 * (A - B / m) + 0.5
    float g = clamp((m - dp.z) / (dp.w - dp.z), 0.0, 1.0);
    g = 1.0 - sqrt(g);
    frag_color = vec4(vec3(g), 1.0);
}
