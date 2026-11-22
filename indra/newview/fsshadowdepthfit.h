/**
 * @file fsshadowdepthfit.h
 * @brief Sun shadow maps fitted to the depth range in view (FSRenderShadowFitDepth)
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

#ifndef FS_SHADOWDEPTHFIT_H
#define FS_SHADOWDEPTHFIT_H

#include "stdtypes.h"

class LLCamera;
class LLRenderTarget;
class LLVertexBuffer;

// Sample distribution shadow maps (Lauritzen 2010), the range part. Once per frame, after the main
// view's g-buffer is drawn, its depth buffer is reduced on the GPU to the nearest and the farthest
// distance of any opaque pixel (the sky left out), in a few passes into a 1x1 target, and that is
// read back through a pack buffer and a fence without waiting for the GPU. The next frames' sun
// shadow splits are then spread between those two distances instead of between the camera's near
// plane and the whole shadow distance (LLPipeline::generateSunShadow), so in a room or a close-up
// the shadow maps cover a few metres and are several times sharper.
//
// The range used grows at once and shrinks smoothly, and is rounded to small steps, so it doesn't
// pump; see getRange. Everything is off, and nothing is built or allocated, while
// FSRenderShadowFitDepth is off.
namespace FSShadowDepthFit
{
    // FSRenderShadowFitDepth, and sun shadows on
    bool enabled();

    // Called at every shader load: drops the reduction programs. They are built again the first
    // time they are needed, so the setting can be switched without a shader rebuild.
    void loadShaders();

    // Main view only, after the g-buffer and before the lighting passes: reduces depth_target's
    // depth (drawn with the current projection) and starts reading the result back. Also takes a
    // finished read back. Leaves the GL state as found.
    void reduce(LLRenderTarget& depth_target, LLVertexBuffer* screen_triangle);

    // Main view's sun shadow, once per frame: the near and far distance to spread the splits over,
    // from the last read back, smoothed. cpu_near and cpu_far are the range the splits would use
    // without the fit; the result never reaches further than cpu_far. False, with the outputs
    // untouched, when the fit has nothing usable (off, no read back yet, nothing but sky in view,
    // a snapshot): the splits are then made as without it.
    bool getRange(const LLCamera& camera, F32 cpu_near, F32 cpu_far, F32& near_out, F32& far_out);

    // True while this frame's sun shadow range ends before the range it would have without the
    // fit: nothing in view lies past the end, so the shaders without the smooth fade skip the
    // fade at the end of the shadow range, which would otherwise fade the shadows on the farthest
    // walls of a room. The smooth fade gets a shorter one instead (fadeLength).
    bool rangeCut();

    // The share of the sun shadow range the smooth far fade (FSRenderShadowSmoothFade) takes this
    // frame, from the one set (fade_length). While the fit cuts the range, a shorter fade that starts
    // past everything the last read back saw, so the far walls of a room keep their shadows but
    // what came into view since the read back gets a soft end, not a hard line.
    F32 fadeLength(F32 fade_length);

    // Frees the targets, the read back buffer and the programs
    void release();
}

#endif // FS_SHADOWDEPTHFIT_H
