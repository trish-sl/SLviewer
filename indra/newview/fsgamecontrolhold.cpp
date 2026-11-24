/**
 * @file fsgamecontrolhold.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsgamecontrolhold.h"

#include "llagentcamera.h"
#include "llframetimer.h"
#include "llgamecontrol.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llui.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#include <algorithm>
#include <cctype>

U32 FSGameControlHold::sActive = 0;
U32 FSGameControlHold::sFired = 0;
F64 FSGameControlHold::sStart[32] = {};
U32 FSGameControlHold::sLastFrame = 0;

// Hold time limits: shorter than this isn't a hold, longer gets tiresome
constexpr F32 FS_GC_HOLD_MIN_SECONDS = 0.2f;
constexpr F32 FS_GC_HOLD_MAX_SECONDS = 3.f;

// static
U32 FSGameControlHold::parseActions(const std::string& list)
{
    // Names for the controller's one-shot actions (LLGameControl::AvatarMiscAction)
    struct Name
    {
        const char* mName;
        U32 mBit;
    };
    static const Name NAMES[] =
    {
        { "sit",       LLGameControl::AVATAR_ACTION_TOGGLE_SIT },  // Toggle sit: sits down or stands up
        { "stand",     LLGameControl::AVATAR_ACTION_TOGGLE_SIT },
        { "fly",       LLGameControl::AVATAR_ACTION_TOGGLE_FLY },
        { "speak",     LLGameControl::AVATAR_ACTION_TOGGLE_SPEAK },
        { "flycam",    LLGameControl::AVATAR_ACTION_TOGGLE_FLYCAM },
        { "mouselook", LLGameControl::AVATAR_ACTION_TOGGLE_MOUSELOOK },
        { "cursor",    LLGameControl::AVATAR_ACTION_TOGGLE_MOUSE_CURSOR },
        { "escape",    LLGameControl::AVATAR_ACTION_ESCAPE },
        { "run",       LLGameControl::AVATAR_ACTION_TOGGLE_RUN },
    };

    U32 mask = 0;
    std::string token;
    auto add_token = [&]()
    {
        for (const Name& name : NAMES)
        {
            if (token == name.mName)
            {
                mask |= name.mBit;
            }
        }
        token.clear();
    };
    for (char c : list)
    {
        if (c == ',' || c == ';' || std::isspace((unsigned char)c))
        {
            add_token();
        }
        else
        {
            token += (char)std::tolower((unsigned char)c);
        }
    }
    add_token();
    return mask;
}

// static
void FSGameControlHold::reset()
{
    sActive = 0;
    sFired = 0;
}

// static
U32 FSGameControlHold::filter(U32 pressed_bits, U32 held_bits)
{
    static LLCachedControl<bool> hold_to_act(gSavedSettings, "FSGameControlHoldToAct", true);
    static LLCachedControl<F32> hold_seconds(gSavedSettings, "FSGameControlHoldSeconds", 1.f);
    static LLCachedControl<std::string> hold_actions(gSavedSettings, "FSGameControlHoldActions", "sit,fly,speak");

    // Not called every frame means the controller stopped driving the avatar for a while (focus
    // lost, game control off, a binding being captured in the preferences): whatever was being
    // held is over, and a button still down doesn't start a new hold without a fresh press.
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame - sLastFrame > 1)
    {
        reset();
    }
    sLastFrame = frame;

    if (!hold_to_act)
    {
        reset();
        return pressed_bits;
    }

    static std::string parsed_list;
    static U32 gated_mask = 0;
    if (parsed_list != hold_actions())
    {
        parsed_list = hold_actions();
        gated_mask = parseActions(parsed_list);
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F64 needed = (F64)llclamp((F32)hold_seconds, FS_GC_HOLD_MIN_SECONDS, FS_GC_HOLD_MAX_SECONDS);

    U32 result = pressed_bits & ~gated_mask;
    for (U32 i = 0; i < 32; ++i)
    {
        const U32 bit = 1u << i;
        if (!(gated_mask & bit))
        {
            // Taken off the list while held
            sActive &= ~bit;
            sFired &= ~bit;
            continue;
        }
        if (sActive & bit)
        {
            if (!(held_bits & bit))
            {
                // Released: early cancels, after firing re-arms for the next press
                sActive &= ~bit;
                sFired &= ~bit;
            }
            else if (!(sFired & bit) && now - sStart[i] >= needed)
            {
                result |= bit;
                sFired |= bit;
            }
        }
        if ((pressed_bits & bit) && !(sActive & bit))
        {
            sActive |= bit;
            sFired &= ~bit;
            sStart[i] = now;
        }
    }
    return result;
}

// static
F32 FSGameControlHold::getProgress()
{
    const U32 pending = sActive & ~sFired;
    if (!pending || LLFrameTimer::getFrameCount() - sLastFrame > 1)
    {
        return -1.f;
    }
    static LLCachedControl<F32> hold_seconds(gSavedSettings, "FSGameControlHoldSeconds", 1.f);
    const F64 needed = (F64)llclamp((F32)hold_seconds, FS_GC_HOLD_MIN_SECONDS, FS_GC_HOLD_MAX_SECONDS);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    F64 longest = 0.0;
    for (U32 i = 0; i < 32; ++i)
    {
        if (pending & (1u << i))
        {
            longest = std::max(longest, now - sStart[i]);
        }
    }
    return llclamp((F32)(longest / needed), 0.f, 0.999f);
}

// static
void FSGameControlHold::draw()
{
    const F32 progress = getProgress();
    if (progress < 0.f || !gViewerWindow)
    {
        return;
    }

    // At the cursor's hot spot, or the crosshair in mouselook where the cursor is hidden
    F32 x;
    F32 y;
    if (gAgentCamera.cameraMouselook())
    {
        const LLRect& world = gViewerWindow->getWorldViewRectScaled();
        x = (F32)world.getCenterX();
        y = (F32)world.getCenterY();
    }
    else
    {
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        x = (F32)mouse.mX;
        y = (F32)mouse.mY;
    }

    constexpr F32 OUTER_RADIUS = 22.f;
    constexpr F32 INNER_RADIUS = 17.f;
    constexpr S32 FULL_STEPS = 48;

    static LLUIColor fill_color = LLUIColorTable::instance().getColor("EmphasisColor", LLColor4::white);
    const LLColor4 fill = fill_color.get();
    const LLColor4 track(0.f, 0.f, 0.f, 0.4f);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLUI::pushMatrix();
    LLUI::translate(x, y);
    {
        // A dark track behind the fill, a pixel wider, so the ring reads on any background
        gl_washer_segment_2d(OUTER_RADIUS + 1.f, INNER_RADIUS - 1.f, 0.f, F_TWO_PI, FULL_STEPS, track, track);
        if (progress > 0.f)
        {
            // Fills clockwise from 12 o'clock
            const S32 steps = std::max(2, (S32)(FULL_STEPS * progress));
            gl_washer_segment_2d(OUTER_RADIUS, INNER_RADIUS, F_PI_BY_TWO, F_PI_BY_TWO - F_TWO_PI * progress, steps, fill, fill);
        }
    }
    LLUI::popMatrix();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
}
