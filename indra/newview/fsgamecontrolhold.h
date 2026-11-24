/**
 * @file fsgamecontrolhold.h
 * @brief Game controller buttons that must be held for a moment before their action fires
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

#ifndef FS_GAMECONTROLHOLD_H
#define FS_GAMECONTROLHOLD_H

#include "stdtypes.h"

#include <string>

// Hold to perform action (FSGameControlHoldToAct): a game controller button bound to a
// one-shot action listed in FSGameControlHoldActions (sit or stand, fly, speak by default)
// has to be held for FSGameControlHoldSeconds before the action fires, like holding a button
// to interact in a game. Releasing it earlier cancels; one fire per press. While a button is
// being held a ring around the mouse cursor fills up.
//
// Only the controller's one-shot actions (LLGameControl::AvatarMiscAction bits) go through
// here; movement, camera and flycam motion, the keyboard and the mouse never do.
class FSGameControlHold
{
public:
    // Called once a frame from LLAgent::applyExternalActions() with the pressed-this-frame
    // (edge) and held (level) AvatarMiscAction bits. Returns the edge bits to act on this
    // frame: actions not gated pass straight through, gated ones are held back until their
    // button has been held long enough, then reported once.
    static U32 filter(U32 pressed_bits, U32 held_bits);

    // How far the longest running hold is, [0, 1), or a negative value with nothing being held
    // (or when filter() wasn't called last frame: the controller isn't driving the avatar).
    static F32 getProgress();

    // Draws the progress ring at the mouse cursor (the screen centre in mouselook). Called
    // from LLViewerWindow::draw() in UI coordinates, on the main thread.
    static void draw();

    // A hold is running: the ring changes every frame (keeps the capped UI redrawing).
    static bool isHolding() { return getProgress() >= 0.f; }

private:
    static U32 parseActions(const std::string& list);
    static void reset();

    static U32 sActive;         // gated bits whose press is being timed or already fired
    static U32 sFired;          // gated bits that fired on this press (wait for the release)
    static F64 sStart[32];      // when each active bit's button went down
    static U32 sLastFrame;      // frame filter() last ran
};

#endif // FS_GAMECONTROLHOLD_H
