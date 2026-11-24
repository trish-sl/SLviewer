/**
 * @file fsavatartribudget.h
 * @brief A triangle budget per avatar (FSRenderAvatarTriangleBudget): the largest rigged meshes step down a detail level
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

#ifndef FS_AVATARTRIBUDGET_H
#define FS_AVATARTRIBUDGET_H

#include "stdtypes.h"

// With FSRenderAvatarTriangleBudget above 0, each other avatar (not you, not animesh) drawn in full is held to
// about that many triangles across its rigged meshes: once a frame, the meshes' triangles at the detail the
// camera's distance gives them (LLVOVolume::calcLOD's mFSUnbiasedLOD, the mesh header's estimate per level)
// are summed, and while the sum is over the budget the mesh with the most triangles is stepped down one
// level (LLVOVolume::mFSLODBias, which calcLOD takes off the level it computes). The steps are a function of
// the camera distance and the meshes alone, so they don't flap. A mesh at its lowest level can't go further;
// the budget is then overrun. Mesh headers not loaded yet count at their current triangles and aren't
// stepped. Main thread, before the frame's LOD updates.
namespace FSAvatarTriangleBudget
{
    // Every frame (llappviewer.cpp). log_enabled: with FSRenderGPUTimings, a line every interval naming the
    // avatars over budget and what was stepped down.
    void frame(bool log_enabled, F32 interval_seconds);
}

#endif // FS_AVATARTRIBUDGET_H
