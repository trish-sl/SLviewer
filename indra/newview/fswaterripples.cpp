/**
 * @file fswaterripples.cpp
 * @brief Interactive water: ripples, wakes, entry splashes and churned-water foam (FSRenderWaterRipples)
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

#include "fswaterripples.h"

#include "fswaterobstacles.h"
#include "fswindfield.h"
#include "llagent.h"
#include "llcharacter.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

extern bool gCubeSnapshot;
extern bool gSnapshot;

namespace
{
    // Must match FS_RIPPLE_MAX_SOURCES in fsWaterRippleSimF.glsl
    constexpr S32 MAX_SOURCES = 64;

    // Fixed simulation step. A slow frame runs at most MAX_STEPS of them and drops the rest of its
    // time, so the waves slow down for a moment instead of the frame getting slower still.
    constexpr F64 STEP = 1.0 / 60.0;
    constexpr S32 MAX_STEPS = 4;

    // Longer without a step (water out of view, a stall, a snapshot) and the sources start over:
    // no wake drawn across the gap, no splash for whoever crossed the surface meanwhile
    constexpr F64 GAP_SECONDS = 1.0;

    // A source not seen for this long is forgotten
    constexpr F64 TRACK_SECONDS = 2.0;

    // Further than this between two steps is a jump (teleport, sitting down), not a path
    constexpr F64 JUMP_METRES = 6.0;

    // Moving objects: slower than this (m/s) and they are left out. Avatars always count.
    constexpr F32 MOVING_SPEED = 0.2f;

    // Height pushed down per second, per m/s of speed, at the centre of a 35 cm footprint. It
    // grows with the speed again (up to 4x at 6 m/s), so a runner churns the water more than a
    // walker instead of leaving the same wake spread thinner.
    constexpr F32 MOVE_RATE = 0.03f;
    // Moving objects (boats, vehicles, floats) push this much harder than an avatar of their size
    // would: a hull displaces water along its whole length, and at the avatar's rate their wakes
    // read as too faint. Avatars keep 1.
    constexpr F32 OBJECT_WAKE_GAIN = 1.22f;
    // Idle bob of an avatar standing or swimming in the water: rate amplitude and frequency
    constexpr F32 IDLE_RATE = 0.015f;
    constexpr F32 IDLE_HZ = 0.6f;
    // Entry splash: depth of the impulse at strength 1, and how much wider than the footprint
    constexpr F32 SPLASH_DEPTH = 0.05f;
    constexpr F32 SPLASH_WIDTH = 1.8f;

    // Visual gain on the simulated slope at strength 1
    constexpr F32 SLOPE_GAIN = 2.5f;
    // Churned-water foam: slope sensitivity at strength 1, and how long foam takes to fade to 5 %
    constexpr F32 FOAM_GAIN = 6.f;
    constexpr F64 FOAM_SECONDS = 1.5;

    // A grid that could not be allocated is not tried again every frame
    constexpr F64 FAILED_RETRY_SECONDS = 30.0;

    // The region wind (FSRenderWindEffects), at FSRenderWaterWindStrength 1:
    // - Catspaws: each step pushes a faint noise into the height, fine noise under slow, larger
    //   gust patches, both world anchored and carried downwind by the wind's drift. The simulation
    //   turns it into moving ripple texture that mixes with the wakes. No push below WIND_CALM,
    //   full from WIND_FULL (m/s).
    constexpr F32 WIND_CALM = 0.5f;
    constexpr F32 WIND_FULL = 10.f;
    // Size of the push with full cover (m). Each step pushes this times the share of height the
    // damping takes per step, so the ripples level off where push and damping balance, about the
    // same whatever the damping, and never build up (no damping: no push). At the defaults they
    // settle near 2 mm rms with slopes of a few %, peaking near a walker's wake in gust patches.
    constexpr F32 WIND_PUSH = 0.04f;
    // Noise sizes in metres, and the share of the wind's drift each moves with. The fine layers
    // creep (ripples hardly travel with the wind), the gust patches sweep across.
    constexpr F64 WIND_FINE_1 = 1.0;
    constexpr F64 WIND_FINE_2 = 0.5;
    constexpr F64 WIND_PATCH = 14.0;
    constexpr F64 WIND_FINE_1_DRIFT = 0.12;
    constexpr F64 WIND_FINE_2_DRIFT = 0.25;
    constexpr F64 WIND_PATCH_DRIFT = 0.7;
    // The shader's noise repeats after this many noise cells (fsWaterRippleSimF.glsl, & 255)
    constexpr F64 WIND_NOISE_PERIOD = 256.0;
    // - Surface current: the top of the water moves with the wind at a few % of its speed and
    //   carries the ripples and wakes with it; foam rides on top and goes a little faster.
    constexpr F32 SURFACE_DRIFT = 0.03f;
    constexpr F32 FOAM_DRIFT = 0.02f;           // on top of SURFACE_DRIFT
    constexpr F32 MAX_SURFACE_SPEED = 0.6f;     // m/s, at the strongest settings

    // Obstacles (FSRenderWaterRippleObstacles, fswaterobstacles.cpp):
    // - Shallow water over terrain: the share of the height lost per step on top of the damping
    //   where the water is shallowest, fading out over its last metre (fsWaterRippleSimF.glsl).
    //   About three quarters gone in the 0.7 s a ripple takes to cross that metre at the default
    //   speed, so little is left to bounce off the beach.
    constexpr F32 SHALLOW_LOSS = 0.06f;
    // - The wake the surface current sheds off them (FSRenderWaterRippleObstacleWake): the push
    //   per step at WAKE_SPEED (m/s), growing with the current up to twice that, a little under an
    //   avatar's idle bob. The sides take turns at WAKE_HZ, a little faster in a faster current.
    constexpr F32 WAKE_PUSH = 0.0004f;
    constexpr F32 WAKE_SPEED = 0.3f;
    constexpr F64 WAKE_HZ = 0.35;
    constexpr F64 WAKE_HZ_PER_SPEED = 1.5;      // Hz per m/s of current
    // - Foam the current piles up on their upstream side, at WAKE_SPEED (with wake foam on)
    constexpr F32 WAKE_FOAM = 0.35f;
    // - The water line (FSRenderWaterRippleWaterline): laps against obstacles, one at a time at
    //   random (fsWaterRippleSimF.glsl). Lap spots sit WATERLINE_SPACING metres apart (a
    //   jittered, world anchored lattice) and a lap pushes the edge cells within WATERLINE_REACH
    //   metres of its spot, so every bit of edge has a few spots near it. A lap launches a ring
    //   of about WATERLINE_LAP_LENGTH metres wavelength, which takes a push 1.5 times as long as
    //   a wave needs to cross that (about 0.85 s at the default speed). Each spot cuts time into
    //   slots of its own length around WATERLINE_SLOT laps long and laps in a slot with
    //   WATERLINE_CHANCE, at a random moment: a spot laps about every 7 s with no beat to it, and
    //   a half metre post gets a lap somewhere about once a second. A coarse grid
    //   stretches the laps, the spots and their reach alike, so the rings never get shorter than
    //   WATERLINE_MIN_CELLS cells, where the grid only squares and frays them.
    //   WATERLINE_PUSH is the push per step at a lap's peak at strength 1. With the default
    //   damping and cells a half metre post's rings have a slope of about 4 % (rms) a quarter
    //   metre out, 2.5 % at half a metre, 1 % at a metre and next to nothing at two, the bigger
    //   laps several times that close in; a few centimetres high at most where they hit. The
    //   strength goes in as strength^WATERLINE_STRENGTH_EXP, so 3 is about twice 1 rather than
    //   three times. A breeze makes laps up to WATERLINE_WIND more likely all round. The surface
    //   current (counted fully from WAKE_SPEED) makes the sides facing into it lap up to
    //   WATERLINE_UP more often and WATERLINE_UP_STRENGTH harder, and the sides in its lee
    //   WATERLINE_DOWN less often and WATERLINE_DOWN_STRENGTH softer (the wake handles the lee).
    constexpr F32 WATERLINE_PUSH = 0.0026f;
    constexpr F64 WATERLINE_LAP_LENGTH = 0.8;
    constexpr F64 WATERLINE_MIN_CELLS = 4.5;
    constexpr F64 WATERLINE_SPACING = 0.5;
    constexpr F64 WATERLINE_REACH = 0.45;
    constexpr F64 WATERLINE_SLOT = 1.6;
    constexpr F32 WATERLINE_CHANCE = 0.2f;
    constexpr F32 WATERLINE_STRENGTH_EXP = 0.63f;
    constexpr F32 WATERLINE_WIND = 0.5f;
    constexpr F32 WATERLINE_UP = 1.2f;
    constexpr F32 WATERLINE_DOWN = 0.7f;
    constexpr F32 WATERLINE_UP_STRENGTH = 0.5f;
    constexpr F32 WATERLINE_DOWN_STRENGTH = 0.3f;
    // The laps' clock wraps after this many seconds to keep its precision in the shader (a lap
    // cut short once every half hour)
    constexpr F64 WATERLINE_CLOCK_WRAP = 2048.0;
    // - The touch of white where the water meets an obstacle (fsWaterRipplesF.glsl): foam cover at
    //   its brightest at strength 1, gone by WATERLINE_LINE_FAR metres from the camera, and faded
    //   out on cells too coarse to draw a thin line (WATERLINE_LINE_CELL, m)
    constexpr F32 WATERLINE_LINE = 0.22f;
    constexpr F32 WATERLINE_LINE_FAR = 45.f;
    constexpr F32 WATERLINE_LINE_CELL = 0.3f;

    LLGLSLShader sSimProgram;

    // Two maps in turn: r height (m), g height a step earlier, b foam. Never destroyed at exit:
    // released with the GL buffers.
    LLRenderTarget* sMaps[2] = { nullptr, nullptr };
    S32 sCurrent = 0;      // the map holding the latest state
    bool sValid = false;   // the maps hold a state for the grid below (cleared, or stepped since)
    S32 sRes = 0;          // cells across
    F64 sCell = 0.0;       // cell size, metres
    S64 sOriginX = 0;      // grid corner, in whole cells of global coordinates
    S64 sOriginY = 0;
    F64 sFailedAt = -1.0;

    // How far the surface current has carried the water (global metres, x east, y north). The
    // grid lives in the water's frame, world position minus this, so the current moves its
    // content smoothly, without resampling it: the whole-cell grid shift does the rest. Zero
    // while the wind is off.
    LLVector3d sCurrentOffset;

    F64 sWakePhase = 0.0;      // the obstacles' wake shedding, radians
    F64 sWaterlineClock = 0.0; // the water line's laps, simulated seconds (wrapped)

    F64 sLastTime = -1.0;      // when update last ran for the main view
    F64 sLastStepTime = -1.0;  // when the simulation last stepped
    F64 sAccum = 0.0;          // simulated time owed
    U32 sLastFrame = U32_MAX;

    // What is remembered of a source between steps: where it was, so its wake joins up, and
    // whether it was in the water, so crossing the surface splashes once. Keyed by id, never by
    // pointer, so a source that dies or leaves is simply not found again.
    struct Track
    {
        LLVector3d mLast;
        F64 mSeen = 0.0;
        bool mInWater = false;
    };
    std::unordered_map<LLUUID, Track> sTracks;

    struct Source
    {
        LLVector3d mFrom;
        LLVector3d mTo;
        F32 mRadius;
        F32 mAmount;   // metres added at the centre this step (negative pushes down)
        F64 mDist2;    // horizontal distance to the camera, squared
    };

    struct Settings
    {
        F32 mSpeed;
        F32 mDamping;
        F32 mSplash;
        S32 mMaxSources;
        bool mFoam;
        F32 mFoamStrength;
        bool mWake;
        F32 mWaterline;  // FSRenderWaterRippleWaterline strength, 0 = off
    };

    LLRenderTarget& map(S32 i)
    {
        if (!sMaps[i])
        {
            sMaps[i] = new LLRenderTarget();
        }
        return *sMaps[i];
    }

    bool mapsComplete()
    {
        return sMaps[0] && sMaps[1] && sMaps[0]->isComplete() && sMaps[1]->isComplete();
    }

    void releaseMaps()
    {
        for (LLRenderTarget* target : sMaps)
        {
            if (target)
            {
                target->release();
            }
        }
        sValid = false;
    }

    // AMD doesn't zero new targets: flat water in both maps before either is read
    void clearMaps()
    {
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gGL.setColorMask(true, true);
        const GLfloat zero[4] = { 0.f, 0.f, 0.f, 0.f };
        for (LLRenderTarget* target : sMaps)
        {
            target->bindTarget();
            glClearBufferfv(GL_COLOR, 0, zero);
            target->flush();
        }
        sCurrent = 0;
    }

    bool ensureMaps(S32 res, F64 now)
    {
        if (mapsComplete() && (S32)sMaps[0]->getWidth() == res && (S32)sMaps[1]->getWidth() == res)
        {
            return true;
        }
        if (sFailedAt >= 0.0 && now - sFailedAt < FAILED_RETRY_SECONDS)
        {
            return false;
        }
        releaseMaps();
        if (!map(0).allocate(res, res, GL_RGBA16F, false) || !map(1).allocate(res, res, GL_RGBA16F, false))
        {
            LL_WARNS("Render") << "Water ripples: a " << res << "x" << res << " grid could not be allocated" << LL_ENDL;
            releaseMaps();
            sFailedAt = now;
            return false;
        }
        sFailedAt = -1.0;
        sValid = false; // cleared before use
        return true;
    }

    // Brings a source's track up to date and, when it is in the water, adds what it does to the
    // surface this step
    void addSource(std::vector<Source>& out, const LLUUID& id, const LLVector3d& pos, const LLVector3d& camera,
                   bool in_water, F32 radius, F32 speed, bool idle_bob, F32 sim_dt, F64 now, F32 splash, F32 gain)
    {
        Track& track = sTracks[id];
        const bool fresh = track.mSeen <= 0.0;
        LLVector3d from = fresh ? pos : track.mLast;
        if ((pos - from).magVecSquared() > JUMP_METRES * JUMP_METRES)
        {
            from = pos;
        }
        // Already in the water when first seen (arriving, the grid starting over): no splash
        const bool entered = !fresh && in_water && !track.mInWater;
        track.mLast = pos;
        track.mSeen = now;
        track.mInWater = in_water;
        if (!in_water)
        {
            return;
        }

        F32 amount = 0.f;
        if (speed > 0.1f)
        {
            const F32 size = llclamp(sqrtf(radius / 0.35f), 0.5f, 3.f);
            amount -= MOVE_RATE * speed * llclamp(speed / 1.5f, 0.3f, 4.f) * size * sim_dt;
        }
        if (idle_bob)
        {
            // Each avatar bobs at its own phase, so a crowd doesn't pulse together
            const F32 phase = (F32)(id.getDigest64() % 1024) * (F_TWO_PI / 1024.f);
            const F32 t = (F32)fmod(now, 1000.0);
            amount += IDLE_RATE * sinf(t * IDLE_HZ * F_TWO_PI + phase) * sim_dt;
        }
        if (entered && splash > 0.f)
        {
            amount -= SPLASH_DEPTH * splash * llclamp(0.5f + speed * 0.25f, 0.5f, 2.f);
            radius *= SPLASH_WIDTH;
        }
        amount *= gain; // the simulation clamps each source's push (fs_ripple_src.y) to +-0.5 m
        if (amount == 0.f || !std::isfinite(amount))
        {
            return;
        }

        const F64 dx = pos.mdV[VX] - camera.mdV[VX];
        const F64 dy = pos.mdV[VY] - camera.mdV[VY];
        out.push_back({ from, pos, radius, amount, dx * dx + dy * dy });
    }

    // The avatars and moving objects near the camera, nearest first, at most max_sources. Cheap:
    // the avatar list and the active (moving) object list, no geometry.
    void gatherSources(std::vector<Source>& out, const LLVector3d& camera, F64 half_extent, F32 sim_dt, F64 now,
                       const Settings& settings)
    {
        out.clear();
        const F64 reach = half_extent + 8.0;

        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() || avatar->mDrawable.isNull())
            {
                continue;
            }
            LLViewerRegion* region = avatar->getRegion();
            if (!region)
            {
                continue;
            }
            const LLVector3d pos = avatar->getPositionGlobal();
            if (fabs(pos.mdV[VX] - camera.mdV[VX]) > reach || fabs(pos.mdV[VY] - camera.mdV[VY]) > reach)
            {
                continue;
            }
            // Seated: the seat makes the wake when it moves (a boat, a vehicle: the object loop below).
            // On a still seat the avatar makes none either: the sit pose's pelvis height puts the
            // feet of someone on a deck just above the water below its surface, which rippled and
            // splashed as they sat down.
            if (avatar->isSitting())
            {
                continue;
            }
            F32 height = avatar->mBodySize.mV[VZ];
            if (!(height > 0.5f) || height > 4.f)
            {
                height = 1.8f;
            }
            const F32 foot = (F32)pos.mdV[VZ] - avatar->getPelvisToFoot();
            const F32 water = region->getWaterHeight();
            const bool in_water = foot < water + 0.05f && foot + height > water - 0.1f;
            const F32 radius = llclamp(height * 0.18f, 0.25f, 0.6f);
            addSource(out, avatar->getID(), pos, camera, in_water, radius, avatar->getVelocity().length(), true, sim_dt, now,
                      settings.mSplash, 1.f);
        }

        for (const LLPointer<LLViewerObject>& ptr : gObjectList.getActiveObjects())
        {
            LLViewerObject* object = ptr.get();
            if (!object || object->isDead() || object->isAvatar() || object->getParent() || object->isAttachment() ||
                object->mDrawable.isNull())
            {
                continue;
            }
            const F32 speed = object->getVelocity().length();
            if (!(speed > MOVING_SPEED))
            {
                continue;
            }
            LLViewerRegion* region = object->getRegion();
            if (!region)
            {
                continue;
            }
            const LLVector3d pos = object->getPositionGlobal();
            if (fabs(pos.mdV[VX] - camera.mdV[VX]) > reach || fabs(pos.mdV[VY] - camera.mdV[VY]) > reach)
            {
                continue;
            }
            // The whole linkset's box when it moves as one (its bridge), agent space
            LLDrawable* drawable = object->mDrawable;
            LLSpatialBridge* bridge = drawable->getSpatialBridge();
            const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : drawable->getSpatialExtents();
            const F32 size_x = ext[1][0] - ext[0][0];
            const F32 size_y = ext[1][1] - ext[0][1];
            if (!(size_x >= 0.f && size_y >= 0.f && size_x < 256.f && size_y < 256.f))
            {
                continue; // not computed yet, or not finite
            }
            const F32 water = region->getWaterHeight();
            const bool in_water = ext[0][2] < water && ext[1][2] > water;
            const F32 radius = llclamp(0.5f * llmin(size_x, size_y), 0.3f, 4.f);
            addSource(out, object->getID(), pos, camera, in_water, radius, speed, false, sim_dt, now, settings.mSplash,
                      OBJECT_WAKE_GAIN);
        }

        const size_t max_sources = (size_t)settings.mMaxSources; // clamped by readSettings
        if (out.size() > max_sources)
        {
            std::nth_element(out.begin(), out.begin() + max_sources, out.end(),
                             [](const Source& a, const Source& b) { return a.mDist2 < b.mDist2; });
            out.resize(max_sources);
        }

        // Forget what has not been seen for a while (gone, out of range)
        for (auto it = sTracks.begin(); it != sTracks.end();)
        {
            if (now - it->second.mSeen > TRACK_SECONDS)
            {
                it = sTracks.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    Settings readSettings()
    {
        static LLCachedControl<F32> speed(gSavedSettings, "FSRenderWaterRipplesSpeed", 1.4f);
        static LLCachedControl<F32> damping(gSavedSettings, "FSRenderWaterRipplesDamping", 0.6f);
        static LLCachedControl<F32> splash(gSavedSettings, "FSRenderWaterRipplesSplash", 1.f);
        static LLCachedControl<S32> max_sources(gSavedSettings, "FSRenderWaterRipplesMaxSources", 32);
        static LLCachedControl<bool> foam(gSavedSettings, "FSRenderWaterRippleFoam", false);
        static LLCachedControl<F32> foam_strength(gSavedSettings, "FSRenderWaterRippleFoamStrength", 1.f);
        static LLCachedControl<bool> wake(gSavedSettings, "FSRenderWaterRippleObstacleWake", false);
        Settings settings;
        settings.mSpeed = llclamp((F32)speed, 0.1f, 10.f);
        settings.mDamping = llclamp((F32)damping, 0.f, 0.99f);
        settings.mSplash = llclamp((F32)splash, 0.f, 4.f);
        settings.mMaxSources = llclamp((S32)max_sources, 1, MAX_SOURCES);
        settings.mFoam = foam;
        settings.mFoamStrength = llclamp((F32)foam_strength, 0.f, 4.f);
        settings.mWake = wake;
        static LLCachedControl<bool> waterline(gSavedSettings, "FSRenderWaterRippleWaterline", true);
        static LLCachedControl<F32> waterline_strength(gSavedSettings, "FSRenderWaterRippleWaterlineStrength", 1.f);
        settings.mWaterline = waterline ? llclamp((F32)waterline_strength, 0.f, 3.f) : 0.f;
        return settings;
    }

    // What the region wind does to the water this frame (all off unless both FSRenderWindEffects
    // and the ripples are on, and FSRenderWaterWindStrength is above 0)
    struct Wind
    {
        bool mOn = false;
        F32 mCover = 0.f;           // catspaw amount: 0 in still air, the strengths at WIND_FULL
        F32 mGust = 0.f;            // FSWindField's gusts, 0 to 1
        LLVector3 mCurrent;         // surface current, m/s, x east, y north
        LLVector3 mFoamCurrent;     // the foam's extra speed on top of it
        LLVector3d mDrift;          // FSWindField's drift, metres
    };

    Wind readWind()
    {
        static LLCachedControl<F32> water_wind(gSavedSettings, "FSRenderWaterWindStrength", 1.f);
        Wind wind;
        const FSWindField::State& state = FSWindField::get();
        const F32 amount = llclamp(state.mStrength * llclamp((F32)water_wind, 0.f, 2.f), 0.f, 2.f);
        LLVector3 velocity = state.mVelocity;
        velocity.mV[VZ] = 0.f;
        const F32 speed = velocity.length();
        if (!state.mActive || !(amount > 0.f) || !std::isfinite(speed) || !state.mDrift.isFinite())
        {
            return wind;
        }
        const F32 t = llclamp((speed - WIND_CALM) / (WIND_FULL - WIND_CALM), 0.f, 1.f);
        wind.mOn = true;
        wind.mCover = t * t * (3.f - 2.f * t) * amount;
        wind.mGust = llclamp(state.mGust, 0.f, 1.f);
        wind.mDrift = state.mDrift;
        wind.mCurrent = velocity * (SURFACE_DRIFT * amount);
        wind.mFoamCurrent = velocity * (FOAM_DRIFT * amount);
        const F32 current = wind.mCurrent.length();
        if (current > MAX_SURFACE_SPEED)
        {
            const F32 scale = MAX_SURFACE_SPEED / current;
            wind.mCurrent *= scale;
            wind.mFoamCurrent *= scale;
        }
        return wind;
    }

    // Wraps a noise coordinate into [0, WIND_NOISE_PERIOD): the shader's noise repeats there, so
    // the pattern never jumps, and the float the shader gets keeps its precision
    F32 wrapNoise(F64 v)
    {
        F64 w = fmod(v, WIND_NOISE_PERIOD);
        if (w < 0.0)
        {
            w += WIND_NOISE_PERIOD;
        }
        return (F32)w;
    }

    // A noise layer of 'size' metres is left out when it would be under a few cells (it could
    // only alias), and fades in above
    F32 noiseWeight(F64 size)
    {
        const F32 t = llclamp((F32)((size / sCell - 2.5) / 2.5), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    void gridSettings(S32& res, F64& cell)
    {
        static LLCachedControl<F32> grid_size(gSavedSettings, "FSRenderWaterRipplesGridSize", 64.f);
        static LLCachedControl<S32> resolution(gSavedSettings, "FSRenderWaterRipplesResolution", 512);
        res = llclamp((S32)resolution, 128, 1024);
        cell = (F64)llclamp((F32)grid_size, 16.f, 256.f) / (F64)res;
    }

    // Runs 'steps' simulation steps: the grid shift and the sources go into the first
    void simulate(LLVertexBuffer* screen_triangle, S32 steps, S32 shift_x, S32 shift_y, const std::vector<Source>& sources,
                  const Settings& settings, const Wind& wind)
    {
        static LLStaticHashedString s_wind("fs_ripple_wind");
        static LLStaticHashedString s_wind_o1("fs_ripple_wind_o1");
        static LLStaticHashedString s_wind_o2("fs_ripple_wind_o2");
        static LLStaticHashedString s_wind_s("fs_ripple_wind_s");
        static LLStaticHashedString s_prev("fs_ripple_prev");
        static LLStaticHashedString s_shift("fs_ripple_shift");
        static LLStaticHashedString s_sim("fs_ripple_sim");
        static LLStaticHashedString s_cell("fs_ripple_cell");
        static LLStaticHashedString s_count("fs_ripple_count");
        static LLStaticHashedString s_seg("fs_ripple_seg");
        static LLStaticHashedString s_src("fs_ripple_src");
        static LLStaticHashedString s_obst_map("fs_ripple_obst_map");
        static LLStaticHashedString s_obst("fs_ripple_obst");
        static LLStaticHashedString s_wake("fs_ripple_wake");
        static LLStaticHashedString s_wake_osc("fs_ripple_wake_osc");
        static LLStaticHashedString s_waterline("fs_ripple_waterline");
        static LLStaticHashedString s_waterline_o("fs_ripple_waterline_o");
        static LLStaticHashedString s_waterline_f("fs_ripple_waterline_f");
        static LLStaticHashedString s_waterline_g("fs_ripple_waterline_g");

        // Waves cross (speed * step / cell) cells a step. The explicit scheme is only stable below
        // 1/sqrt(2) of a cell; very fine cells hold the speed down rather than blow up.
        const F64 courant = llmin((F64)settings.mSpeed * STEP / sCell, 0.67);
        const F32 k = (F32)(courant * courant);
        // Amplitude keeps sqrt(keep) per step: (1 - damping) per second
        const F32 keep = (F32)pow(1.0 - (F64)settings.mDamping, 2.0 * STEP);
        const F32 foam_keep = (F32)pow(0.05, STEP / FOAM_SECONDS);
        const F32 foam_gain = settings.mFoam ? FOAM_GAIN * settings.mFoamStrength : 0.f;

        // Wind: catspaw push per step and the noise it is shaped by, and the foam's extra drift
        // in cells per step (the bilinear read it needs is under a cell)
        GLfloat wind_params[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat wind_o1[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat wind_o2[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat wind_s[3] = { 0.f, 0.f, 0.f };
        if (wind.mOn)
        {
            // The share of the height the damping takes away per step (see WIND_PUSH)
            const F32 damped = 1.f - (F32)pow(1.0 - (F64)settings.mDamping, STEP);
            const F32 weight_1 = noiseWeight(WIND_FINE_1);
            const F32 weight_2 = noiseWeight(WIND_FINE_2);
            const F32 push = WIND_PUSH * damped * wind.mCover;
            if (push > 0.f && std::isfinite(push) && weight_1 + weight_2 > 0.f)
            {
                // World position of the grid's corner, then each layer's noise coordinate there
                const F64 world_x = (F64)sOriginX * sCell + sCurrentOffset.mdV[VX];
                const F64 world_y = (F64)sOriginY * sCell + sCurrentOffset.mdV[VY];
                const F64 drift_x = wind.mDrift.mdV[VX];
                const F64 drift_y = wind.mDrift.mdV[VY];
                wind_params[0] = push;
                wind_params[1] = wind.mGust;
                // The layers' lattices are offset, so they don't line up at the corner
                wind_o1[0] = wrapNoise((world_x - drift_x * WIND_FINE_1_DRIFT) / WIND_FINE_1);
                wind_o1[1] = wrapNoise((world_y - drift_y * WIND_FINE_1_DRIFT) / WIND_FINE_1);
                wind_o1[2] = wrapNoise((world_x - drift_x * WIND_FINE_2_DRIFT) / WIND_FINE_2 + 101.0);
                wind_o1[3] = wrapNoise((world_y - drift_y * WIND_FINE_2_DRIFT) / WIND_FINE_2 + 37.0);
                wind_o2[0] = wrapNoise((world_x - drift_x * WIND_PATCH_DRIFT) / WIND_PATCH + 59.0);
                wind_o2[1] = wrapNoise((world_y - drift_y * WIND_PATCH_DRIFT) / WIND_PATCH + 173.0);
                wind_o2[2] = weight_1;
                wind_o2[3] = weight_2;
                wind_s[0] = (GLfloat)(sCell / WIND_FINE_1);
                wind_s[1] = (GLfloat)(sCell / WIND_FINE_2);
                wind_s[2] = (GLfloat)(sCell / WIND_PATCH);
            }
            if (foam_gain > 0.f)
            {
                wind_params[2] = (GLfloat)llclamp((F64)wind.mFoamCurrent.mV[VX] * STEP / sCell, -0.5, 0.5);
                wind_params[3] = (GLfloat)llclamp((F64)wind.mFoamCurrent.mV[VY] * STEP / sCell, -0.5, 0.5);
            }
        }

        // Sources in cells of the grid as it is after this frame's shift
        GLfloat seg[MAX_SOURCES * 4];
        GLfloat src[MAX_SOURCES * 4];
        // In the water's frame: minus what the surface current carried away (zero without wind)
        const S32 count = llmin((S32)sources.size(), MAX_SOURCES);
        const F64 corner_x = (F64)sOriginX + sCurrentOffset.mdV[VX] / sCell;
        const F64 corner_y = (F64)sOriginY + sCurrentOffset.mdV[VY] / sCell;
        for (S32 i = 0; i < count; ++i)
        {
            const Source& source = sources[i];
            seg[i * 4 + 0] = (GLfloat)(source.mFrom.mdV[VX] / sCell - corner_x);
            seg[i * 4 + 1] = (GLfloat)(source.mFrom.mdV[VY] / sCell - corner_y);
            seg[i * 4 + 2] = (GLfloat)(source.mTo.mdV[VX] / sCell - corner_x);
            seg[i * 4 + 3] = (GLfloat)(source.mTo.mdV[VY] / sCell - corner_y);
            src[i * 4 + 0] = (GLfloat)llmax((F64)source.mRadius / sCell, 1.5);
            src[i * 4 + 1] = (GLfloat)llclamp(source.mAmount, -0.5f, 0.5f);
            src[i * 4 + 2] = 0.f;
            src[i * 4 + 3] = 0.f;
        }

        // Obstacles: where this grid's cells fall in the mask (whole cells, plus the fraction the
        // surface current has carried the water), the shallow-water fade, and the wake the current
        // sheds off them. Nothing without a mask: the simulation runs as without obstacles.
        GLfloat obst[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat wake[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat wake_osc[2] = { 0.f, 0.f };
        GLfloat waterline[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat waterline_o[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat waterline_f[4] = { 0.f, 0.f, 0.f, 0.f };
        GLfloat waterline_g[4] = { 0.f, 0.f, 0.f, 0.f };
        S64 mask_x = 0;
        S64 mask_y = 0;
        const bool obstacles = FSWaterObstacles::enabled();
        LLRenderTarget* mask = obstacles ? FSWaterObstacles::mask(sCell, mask_x, mask_y) : nullptr;
        if (obstacles)
        {
            FSWaterObstacles::noteSimulation(mask != nullptr);
        }
        if (mask)
        {
            obst[0] = (GLfloat)((F64)(sOriginX - mask_x) + sCurrentOffset.mdV[VX] / sCell);
            obst[1] = (GLfloat)((F64)(sOriginY - mask_y) + sCurrentOffset.mdV[VY] / sCell);
            obst[2] = SHALLOW_LOSS;
            obst[3] = 1.f;
            const F32 current = wind.mOn ? wind.mCurrent.length() : 0.f;
            if (settings.mWake && current > 0.01f && std::isfinite(current))
            {
                const F32 amount = llmin(current / WAKE_SPEED, 2.f);
                wake[0] = wind.mCurrent.mV[VX] / current;
                wake[1] = wind.mCurrent.mV[VY] / current;
                wake[2] = WAKE_PUSH * amount;
                wake[3] = foam_gain > 0.f ? llmin(WAKE_FOAM * amount, 0.6f) : 0.f;
                sWakePhase = fmod(sWakePhase + F_TWO_PI * (WAKE_HZ + WAKE_HZ_PER_SPEED * current) * steps * STEP, (F64)F_TWO_PI);
                wake_osc[0] = (GLfloat)sin(sWakePhase);
                wake_osc[1] = (GLfloat)sin(2.0 * sWakePhase);
            }
            if (settings.mWaterline > 0.f)
            {
                const F32 push = WATERLINE_PUSH * powf(settings.mWaterline, WATERLINE_STRENGTH_EXP);
                // The speed the waves really travel at (the step may hold it down)
                const F64 wave_speed = courant * sCell / STEP;
                if (push > 0.f && std::isfinite(push) && wave_speed > 0.0)
                {
                    // How much a coarse grid stretches the laps
                    const F64 stretch = llmax(1.0, WATERLINE_MIN_CELLS * sCell / WATERLINE_LAP_LENGTH);
                    const F64 spacing = WATERLINE_SPACING * stretch;
                    const F64 lap = 1.5 * WATERLINE_LAP_LENGTH * stretch / wave_speed;
                    waterline[0] = push;
                    waterline[1] = (GLfloat)sWaterlineClock; // set again for each step below
                    waterline[2] = (GLfloat)(WATERLINE_SLOT * lap);
                    waterline[3] = (GLfloat)lap;
                    // The lap spots are world anchored: the lattice's coordinates at the grid's
                    // corner (in the world, not the water: the current carries the rings, not
                    // the posts)
                    const F64 world_x = (F64)sOriginX * sCell + sCurrentOffset.mdV[VX];
                    const F64 world_y = (F64)sOriginY * sCell + sCurrentOffset.mdV[VY];
                    waterline_o[0] = wrapNoise(world_x / spacing);
                    waterline_o[1] = wrapNoise(world_y / spacing);
                    waterline_o[2] = (GLfloat)(sCell / spacing);
                    waterline_o[3] = (GLfloat)(WATERLINE_REACH / WATERLINE_SPACING);
                    // More laps in a breeze, never none in still air; the current says which
                    // sides take more of them
                    const F32 breeze = wind.mOn ? llclamp(wind.mCover, 0.f, 1.f) : 0.f;
                    if (current > 0.01f && std::isfinite(current))
                    {
                        const F32 flow = llmin(current / WAKE_SPEED, 1.f) / current;
                        waterline_f[0] = wind.mCurrent.mV[VX] * flow;
                        waterline_f[1] = wind.mCurrent.mV[VY] * flow;
                    }
                    waterline_f[2] = WATERLINE_CHANCE * (1.f + WATERLINE_WIND * breeze);
                    waterline_g[0] = WATERLINE_UP;
                    waterline_g[1] = WATERLINE_DOWN;
                    waterline_g[2] = WATERLINE_UP_STRENGTH;
                    waterline_g[3] = WATERLINE_DOWN_STRENGTH;
                }
            }
        }

        gGL.flush();
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gGL.setColorMask(true, true);

        sSimProgram.bind();
        sSimProgram.uniform4fv(s_obst, 1, obst);
        sSimProgram.uniform4fv(s_wake, 1, wake);
        sSimProgram.uniform2fv(s_wake_osc, 1, wake_osc);
        sSimProgram.uniform4fv(s_waterline, 1, waterline);
        sSimProgram.uniform4fv(s_waterline_o, 1, waterline_o);
        sSimProgram.uniform4fv(s_waterline_f, 1, waterline_f);
        sSimProgram.uniform4fv(s_waterline_g, 1, waterline_g);
        if (mask)
        {
            sSimProgram.uniform1i(s_obst_map, 1);
            mask->bindTexture(0, 1, LLTexUnit::TFO_POINT); // texelFetch
        }
        sSimProgram.uniform1i(s_prev, 0);
        sSimProgram.uniform4f(s_sim, k, keep, foam_keep, foam_gain);
        sSimProgram.uniform2f(s_cell, (F32)sCell, (F32)sRes);
        sSimProgram.uniform4fv(s_wind, 1, wind_params);
        sSimProgram.uniform4fv(s_wind_o1, 1, wind_o1);
        sSimProgram.uniform4fv(s_wind_o2, 1, wind_o2);
        sSimProgram.uniform3fv(s_wind_s, 1, wind_s);
        if (count > 0)
        {
            sSimProgram.uniform4fv(s_seg, count, seg);
            sSimProgram.uniform4fv(s_src, count, src);
        }
        for (S32 step = 0; step < steps; ++step)
        {
            LLRenderTarget& read = *sMaps[sCurrent];
            LLRenderTarget& write = *sMaps[1 - sCurrent];
            write.bindTarget();
            read.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR); // texelFetch: the filter is the one the water samples with
            sSimProgram.uniform2f(s_shift, step == 0 ? (F32)shift_x : 0.f, step == 0 ? (F32)shift_y : 0.f);
            sSimProgram.uniform1i(s_count, step == 0 ? count : 0);
            if (waterline[0] > 0.f)
            {
                // The laps keep time by the step, not the frame
                waterline[1] = (GLfloat)sWaterlineClock;
                sSimProgram.uniform4fv(s_waterline, 1, waterline);
            }
            sWaterlineClock = fmod(sWaterlineClock + STEP, WATERLINE_CLOCK_WRAP);
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
            write.flush();
            sCurrent = 1 - sCurrent;
        }
        if (mask)
        {
            gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        sSimProgram.unbind();
    }
}

bool FSWaterRipples::enabled()
{
    static LLCachedControl<bool> ripples(gSavedSettings, "FSRenderWaterRipples", false);
    return ripples;
}

void FSWaterRipples::loadShaders(S32 shader_level)
{
    sSimProgram.unload();
    FSWaterObstacles::loadShaders(enabled() ? shader_level : 0); // unloaded and freed while off
    if (!enabled() || shader_level <= 0)
    {
        release(); // switched off: give the memory back
        return;
    }

    sSimProgram.mName = "FS Water Ripples Simulation";
    sSimProgram.mShaderFiles.clear();
    sSimProgram.clearPermutations();
    sSimProgram.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
    sSimProgram.mShaderFiles.push_back(std::make_pair("environment/fsWaterRippleSimF.glsl", GL_FRAGMENT_SHADER));
    if (FSWaterObstacles::enabled())
    {
        sSimProgram.addPermutation("FS_WATER_RIPPLE_OBSTACLES", "1");
    }
    sSimProgram.mShaderLevel = shader_level;
    if (!sSimProgram.createShader())
    {
        LL_WARNS("ShaderLoading") << "FS Water Ripples Simulation failed to build, the water stays still" << LL_ENDL;
        sSimProgram.unload();
    }
}

void FSWaterRipples::update(LLVertexBuffer* screen_triangle)
{
    FSWaterObstacles::logStats(LLFrameTimer::getElapsedSeconds()); // FSRenderWaterRippleStats, every 10 s
    if (!enabled())
    {
        if (sValid || (sMaps[0] && sMaps[0]->isComplete()) || !sTracks.empty())
        {
            release();
        }
        return;
    }
    // Main view only, once a frame
    if (gCubeSnapshot || LLPipeline::sRenderingHUDs || !screen_triangle || !sSimProgram.isComplete())
    {
        return;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == sLastFrame)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // Frames went by without an update for over GAP_SECONDS: the water was out of view (the water
    // pool updates only while it draws water). It comes back flat, not with the waves it had when
    // it went. One long frame (a stall) skips no frame and keeps its waves, as before.
    const bool was_unseen = sLastFrame != U32_MAX && frame - sLastFrame > 1 && sLastTime >= 0.0 && now - sLastTime > GAP_SECONDS;
    sLastFrame = frame;

    if (gSnapshot)
    {
        // Frozen: every tile of a hi-res snapshot must see the same water. The time spent is
        // not owed afterwards.
        sLastTime = now;
        return;
    }
    if (was_unseen)
    {
        sValid = false; // cleared below, with the sources
    }
    if (!gAgent.getRegion())
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    S32 res = 0;
    F64 cell = 0.0;
    gridSettings(res, cell);
    if (res != sRes || cell != sCell)
    {
        releaseMaps();
        sTracks.clear();
        sRes = res;
        sCell = cell;
        sFailedAt = -1.0;
    }
    if (!ensureMaps(res, now))
    {
        return;
    }

    // Time owed, in whole steps
    const F64 elapsed = sLastTime < 0.0 ? 0.0 : now - sLastTime;
    sLastTime = now;
    if (sLastStepTime < 0.0 || now - sLastStepTime > GAP_SECONDS)
    {
        sTracks.clear(); // no wakes across the gap
        sAccum = STEP;
    }
    else
    {
        sAccum += llclamp(elapsed, 0.0, 0.25);
    }
    S32 steps = (S32)(sAccum / STEP);
    if (steps > MAX_STEPS)
    {
        steps = MAX_STEPS;
        sAccum = 0.0;
    }
    else
    {
        sAccum -= steps * STEP;
    }
    if (steps <= 0 && sValid)
    {
        return; // faster than the step rate: the sources wait for the next step, their wakes join up
    }

    // The wind carries the water: the surface current moves the grid's frame, in the time
    // simulated this frame
    const Wind wind = readWind();
    if (!sValid)
    {
        sCurrentOffset.setZero();
    }
    else if (wind.mOn && steps > 0)
    {
        sCurrentOffset += LLVector3d(wind.mCurrent) * ((F64)steps * STEP);
    }

    // The grid follows the camera in whole cells, so what is in it stays put in the world (or
    // moves with the surface current). Global coordinates, so crossing into another region
    // moves nothing.
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    bool started_over = false;
    S64 origin_x = (S64)floor((camera.mdV[VX] - sCurrentOffset.mdV[VX]) / sCell) - res / 2;
    S64 origin_y = (S64)floor((camera.mdV[VY] - sCurrentOffset.mdV[VY]) / sCell) - res / 2;
    S64 shift_x = origin_x - sOriginX;
    S64 shift_y = origin_y - sOriginY;
    if (!sValid || llabs(shift_x) >= res / 2 || llabs(shift_y) >= res / 2)
    {
        // Starting, or a jump (teleport, a camera far away): flat water and fresh sources, and
        // nothing left for the current to have carried
        clearMaps();
        sTracks.clear();
        if (sCurrentOffset != LLVector3d::zero)
        {
            sCurrentOffset.setZero();
            origin_x = (S64)floor(camera.mdV[VX] / sCell) - res / 2;
            origin_y = (S64)floor(camera.mdV[VY] / sCell) - res / 2;
        }
        shift_x = 0;
        shift_y = 0;
        sValid = true;
        started_over = true;
    }
    sOriginX = origin_x;
    sOriginY = origin_y;

    // The obstacles' mask, drawn again only when it needs to be (its own GPU timer zone)
    if (FSWaterObstacles::enabled())
    {
        FSWaterObstacles::update(res, sCell, camera, now, started_over);
    }
    if (steps <= 0)
    {
        return;
    }
    sLastStepTime = now;

    const Settings settings = readSettings();
    static std::vector<Source> sources;
    gatherSources(sources, camera, 0.5 * sCell * res, (F32)(steps * STEP), now, settings);

    LL_PROFILE_GPU_ZONE("water ripples sim");
    simulate(screen_triangle, steps, (S32)shift_x, (S32)shift_y, sources, settings, wind);
}

S32 FSWaterRipples::bind(LLGLSLShader& shader, S32 unit)
{
    static LLStaticHashedString s_map("fs_ripple_map");
    static LLStaticHashedString s_grid("fs_ripple_grid");
    static LLStaticHashedString s_params("fs_ripple_params");
    static LLStaticHashedString s_line("fs_ripple_line");
    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderWaterRipplesStrength", 1.f);
    static LLCachedControl<bool> foam(gSavedSettings, "FSRenderWaterRippleFoam", false);
    static LLCachedControl<F32> foam_strength(gSavedSettings, "FSRenderWaterRippleFoamStrength", 1.f);

    if (shader.getUniformLocation(s_params) < 0)
    {
        return -1; // built without FS_WATER_RIPPLES
    }

    const bool unit_ok = unit >= 0 && unit < llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);
    // The sampler always names a free unit, so it never shares one with a sampler of another type
    shader.uniform1i(s_map, unit_ok ? unit : llmax(shader.getTextureChannel(LLShaderMgr::BUMP_MAP), 0));
    if (!unit_ok || !enabled() || !sValid || !mapsComplete() || sRes <= 0)
    {
        shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
        shader.uniform2f(s_line, 0.f, 0.f);
        return -1;
    }

    sMaps[sCurrent]->bindTexture(0, unit, LLTexUnit::TFO_BILINEAR);

    // The grid is in the water's frame: where the surface current has carried it
    const LLVector3 corner = gAgent.getPosAgentFromGlobal(LLVector3d((F64)sOriginX * sCell + sCurrentOffset.mdV[VX],
                                                                     (F64)sOriginY * sCell + sCurrentOffset.mdV[VY], 0.0));
    const F32 size = (F32)(sCell * sRes);
    shader.uniform4f(s_grid, corner.mV[VX], corner.mV[VY], 1.f / size, 1.f / (F32)sRes);

    // Wrapped so the phase keeps its precision; the foam pattern jumps once an hour
    const F32 phase = (F32)fmod(LLFrameTimer::getElapsedSeconds(), 3600.0);
    shader.uniform4f(s_params, llclamp((F32)strength, 0.f, 4.f) * SLOPE_GAIN, (F32)(0.5 / sCell),
                     foam ? llclamp((F32)foam_strength, 0.f, 4.f) : 0.f, phase); // 0 to 4 as the setting: the shader clamps the foam after it

    // The touch of white along the water line: only where the simulation marks obstacles (the
    // map's alpha), and only on cells fine enough to draw it thin
    static LLCachedControl<bool> waterline(gSavedSettings, "FSRenderWaterRippleWaterline", true);
    static LLCachedControl<F32> waterline_strength(gSavedSettings, "FSRenderWaterRippleWaterlineStrength", 1.f);
    F32 line = 0.f;
    if (waterline && FSWaterObstacles::enabled())
    {
        const F32 fine = 1.f - llclamp((F32)(sCell / WATERLINE_LINE_CELL) * 2.f - 1.f, 0.f, 1.f);
        line = WATERLINE_LINE * powf(llclamp((F32)waterline_strength, 0.f, 3.f), WATERLINE_STRENGTH_EXP) * fine;
    }
    shader.uniform2f(s_line, line, 1.f / WATERLINE_LINE_FAR);
    return unit;
}

void FSWaterRipples::release()
{
    FSWaterObstacles::release();
    releaseMaps();
    sTracks.clear();
    sRes = 0;
    sCell = 0.0;
    sCurrentOffset.setZero();
    sWakePhase = 0.0;
    sWaterlineClock = 0.0;
    sAccum = 0.0;
    sLastTime = -1.0;
    sLastStepTime = -1.0;
    sFailedAt = -1.0;
}
