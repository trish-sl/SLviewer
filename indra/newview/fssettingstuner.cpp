/**
 * @file fssettingstuner.cpp
 * @brief "Suggest settings": measures where the frame time goes and suggests settings to change
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

#include "fssettingstuner.h"

#include "fsgputimers.h"
#include "llapp.h"
#include "llbutton.h"
#include "llcharacter.h"
#include "llclipboard.h"
#include "llfloaterpreference.h"
#include "llfloaterreg.h"
#include "lldir.h"
#include "llfile.h"
#include "llgl.h"
#include "llperfstats.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerpartsim.h"
#include "llviewertexture.h"
#include "llvoavatar.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <unordered_map>

bool FSSettingsTuner::sMeasuring = false;

// llviewercontrol.cpp: settings set between these two reload the reflection probes, their GL buffers
// and the shaders once, at the end, instead of once each. Declared here rather than in
// llviewercontrol.h, which most of the viewer includes.
void fs_begin_settings_batch();
void fs_end_settings_batch();

namespace
{
    // A second to let the GPU timers (turned on for the measurement) and the scene settle, then the
    // measurement itself
    constexpr F64 WARMUP_SECONDS = 1.0;
    constexpr F64 CAPTURE_SECONDS = 20.0;
    // The discard bias above this is texture memory running short
    constexpr F64 BIAS_HIGH = 1.3;
    // The main thread's work and the GPU's span both at least this share of the frame: neither has room
    // to spare, and the frame waits for whichever is slower. Seen in the first real use: 18.2 ms frames,
    // the main thread working 16.2 ms (89%), the GPU spanning 17.1 ms (94%). Draw distance cut 0.7 ms
    // of main thread work and the frame stayed at 18.2 ms; the main thread waited 0.9 ms longer for the
    // graphics card instead. At 85% each side is within ~3 ms of an 18 ms frame, about what one
    // suggestion saves, so a cut on one side alone mostly turns into waiting.
    constexpr F64 BALANCED_SHARE = 0.85;
    // A frame time change smaller than this (and than 2% of the frame) is within what two measurements
    // of the same scene differ by
    constexpr F64 COMPARE_NOISE_MS = 0.3;
    // A frame longer than this is a hitch (a shader build, a burst of texture or mesh loading, the
    // window losing focus), not the scene's steady cost: it is left out of the averages and counted
    constexpr F64 HITCH_SECONDS = 0.25;
    // A shader reload or a Render*/FS* setting change during the measurement starts it over; after
    // this many it gives up instead
    constexpr U32 MAX_RESTARTS = 5;

    const char* RULES_FILE = "fs_settings_tuner_rules.xml";
    const char* UNDO_FILE = "fs_settings_tuner_undo.xml";

    enum EState
    {
        STATE_IDLE,
        STATE_WARMUP,
        STATE_CAPTURE,
        STATE_DONE,
        STATE_ABORTED
    };

    struct CPUEntry
    {
        std::string mName;
        F64 mOwnMs = 0.0;
        F64 mTotalMs = 0.0;
    };

    struct Measurement
    {
        F64 mSeconds = 0.0;
        U32 mFrames = 0;
        U32 mHitches = 0;       // frames over HITCH_SECONDS, left out
        F64 mHitchMaxMs = 0.0;
        F64 mFrameMs = 0.0;
        F64 mFps = 0.0;
        F64 mSleepMs = 0.0;     // the frame limiter's sleep
        F64 mSwapMs = 0.0;      // the main thread in the buffer swap (vsync, or the GPU behind)
        F64 mWaitMs = 0.0;      // the main thread waiting for the render thread or a GL sync
        F64 mCPUBusyMs = 0.0;   // the rest
        bool mGPUTimed = false;
        U32 mGPUFrames = 0;
        F64 mGPUFrameMs = 0.0;
        F64 mGPURenderMs = 0.0; // the "Render" zone: from the first to the last draw of the frame
        std::vector<std::pair<std::string, F64>> mGPUZones;     // biggest first
        std::map<std::string, F64> mGPU;
        std::map<std::string, std::pair<F64, F64>> mCPU;        // own, inclusive ms a frame
        F64 mBiasAvg = 0.0;
        F64 mBiasMax = 0.0;
        F64 mBiasHighFrac = 0.0;
        F64 mTightFrac = 0.0;
        F64 mVRAMUsedMB = 0.0;
        F64 mVRAMTargetMB = 0.0;
        F64 mVRAMFreeFrac = 0.0;
        S32 mLiveFreeMB = -1;
        F64 mAvatars = 0.0;
        F64 mFullAvatars = 0.0;
        F64 mVisibleAvatars = 0.0;
        F64 mParticles = 0.0;
        S32 mMaxParticles = 0;
        U32 mFPSLimit = 0;
        bool mVSync = false;
        std::string mBound;     // gpu, cpu, balanced or capped
        bool mMemory = false;
        F64 mHeadroomMs = 0.0;
        std::string mSentence;
        std::map<std::string, F64> mVars;
    };

    EState sState = STATE_IDLE;
    std::string sAbortReason;
    LLTimer sTimer;
    Measurement sResult;

    U32 sFrames = 0;
    F64 sFrameSeconds = 0.0;    // the frames kept
    F64 sLastFrameT = 0.0;
    U32 sHitches = 0;
    F64 sHitchMaxMs = 0.0;
    bool sSkipCPU = false;      // the frame the block timers are about to report was a hitch
    S32 sPendingSleepMs = 0;    // the limiter's sleep at the end of the frame not yet judged
    F64 sSleepMs = 0.0;
    std::unordered_map<const void*, CPUEntry> sCPU;
    U32 sCPUFrames = 0;
    U32 sTexSamples = 0;
    F64 sBiasSum = 0.0;
    F64 sBiasMax = 0.0;
    U32 sBiasHigh = 0;
    U32 sTight = 0;
    F64 sUsedSum = 0.0;
    F64 sTargetSum = 0.0;
    F64 sVisibleSum = 0.0;
    F64 sParticleSum = 0.0;
    U32 sSceneSamples = 0;
    F64 sAvatarSum = 0.0;
    F64 sFullSum = 0.0;
    F64 sNextSceneSample = 0.0;

    // Restarts: what changed, and the Render*/FS* settings watched while measuring
    U32 sRestarts = 0;
    bool sRestartWanted = false;
    std::string sRestartReason;
    std::vector<boost::signals2::connection> sWatched;

    void resetSums()
    {
        sFrames = 0;
        sFrameSeconds = 0.0;
        sLastFrameT = 0.0;
        sHitches = 0;
        sHitchMaxMs = 0.0;
        sSkipCPU = false;
        sPendingSleepMs = 0;
        sSleepMs = 0.0;
        sCPU.clear();
        sCPUFrames = 0;
        sTexSamples = 0;
        sBiasSum = sBiasMax = 0.0;
        sBiasHigh = sTight = 0;
        sUsedSum = sTargetSum = 0.0;
        sVisibleSum = sParticleSum = 0.0;
        sSceneSamples = 0;
        sAvatarSum = sFullSum = 0.0;
        sNextSceneSample = 0.0;
    }

    void settingChanged(LLControlVariable* control)
    {
        if (!sRestartWanted && control)
        {
            sRestartWanted = true;
            sRestartReason = control->getName();
        }
    }

    // A change to a Render* or FS* setting, from Preferences, a preset, a menu or a script command,
    // changes what is measured. Connected for the measurement only, so outside it nothing is
    // watched. FSInternal* (UI state) and LLSD settings (stored data, such as FSGDDArenaPeaks)
    // are not settings of the scene's cost, nor are the ones the viewer writes for itself as it runs:
    // the draw distance saved for the stepping after a teleport, the shader cache's version and
    // the one-shot performance test. (Automatic FPS tuning and that stepping change real settings
    // all the time: the measurement doesn't start while either runs.)
    struct WatchSettings : public LLControlGroup::ApplyFunctor
    {
        void apply(const std::string& name, LLControlVariable* control) override
        {
            if (!control || control->type() == TYPE_LLSD || name.compare(0, 10, "FSInternal") == 0 ||
                (name.compare(0, 6, "Render") != 0 && name.compare(0, 2, "FS") != 0) ||
                name == "FSSavedRenderFarClip" || name == "RenderShaderCacheVersion" || name == "RenderPerformanceTest")
            {
                return;
            }
            sWatched.push_back(control->getSignal()->connect(
                [](LLControlVariable* changed, const LLSD&, const LLSD&) { settingChanged(changed); }));
        }
    };

    void watchSettings()
    {
        if (sWatched.empty())
        {
            WatchSettings watch;
            gSavedSettings.applyToAll(&watch);
        }
    }

    void unwatchSettings()
    {
        for (boost::signals2::connection& connection : sWatched)
        {
            connection.disconnect();
        }
        sWatched.clear();
        sRestartWanted = false;
    }

    void abortMeasure(const std::string& reason)
    {
        unwatchSettings();
        FSGPUTimers::stopCapture();
        FSSettingsTuner::sMeasuring = false;
        sState = STATE_ABORTED;
        sAbortReason = reason;
        sCPU.clear();
    }

    void beginMeasure(bool restart = false)
    {
        if (!restart)
        {
            sRestarts = 0;
            sRestartReason.clear();
            watchSettings();
        }
        sRestartWanted = false;
        resetSums();
        sAbortReason.clear();
        sResult = Measurement();
        sState = STATE_WARMUP;
        sTimer.reset();
        FSSettingsTuner::sMeasuring = true;
        FSGPUTimers::startCapture(); // turns the timers on now; cleared again when the warm-up ends
    }

    void cancelMeasure()
    {
        unwatchSettings();
        if (FSSettingsTuner::sMeasuring)
        {
            FSGPUTimers::stopCapture();
            FSSettingsTuner::sMeasuring = false;
        }
        sCPU.clear();
        sState = STATE_IDLE;
    }

    F64 gpuMs(const Measurement& m, const std::string& zone)
    {
        auto it = m.mGPU.find(zone);
        return it != m.mGPU.end() ? it->second : 0.0;
    }

    F64 cpuOwnMs(const Measurement& m, const std::string& zone)
    {
        auto it = m.mCPU.find(zone);
        return it != m.mCPU.end() ? it->second.first : 0.0;
    }

    F64 cpuTotalMs(const Measurement& m, const std::string& zone)
    {
        auto it = m.mCPU.find(zone);
        return it != m.mCPU.end() ? it->second.second : 0.0;
    }

    void sampleScene()
    {
        S32 avatars = 0;
        S32 full = 0;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() || avatar->isUIAvatar())
            {
                continue;
            }
            ++avatars;
            if (!avatar->isImpostor() && avatar->getOverallAppearance() == LLVOAvatar::AOA_NORMAL)
            {
                ++full;
            }
        }
        sAvatarSum += avatars;
        sFullSum += full;
        sParticleSum += (F64)LLViewerPartSim::sParticleCount2.load();
        ++sSceneSamples;
    }

    // Works out what limits the frame and fills the variables the rules test
    void classify(Measurement& m)
    {
        const F64 frame = m.mFrameMs;
        m.mSwapMs = cpuOwnMs(m, "FS Render Swap");
        m.mWaitMs = cpuOwnMs(m, "FS Wait Render Thread") + cpuOwnMs(m, "FS Wait GL Sync");
        m.mCPUBusyMs = llmax(0.0, frame - m.mSleepMs - m.mSwapMs - m.mWaitMs);
        m.mGPURenderMs = gpuMs(m, "Render");

        const F64 blocked = m.mSwapMs + m.mWaitMs;
        if (m.mSleepMs >= llmax(1.0, frame * 0.08))
        {
            m.mBound = "capped";
            m.mHeadroomMs = m.mSleepMs;
        }
        else if (m.mVSync && m.mSwapMs >= frame * 0.2 && (!m.mGPUTimed || m.mGPURenderMs < frame * 0.8))
        {
            m.mBound = "capped";
            m.mHeadroomMs = m.mSwapMs;
        }
        else if (blocked >= frame * 0.2)
        {
            m.mBound = "gpu";
        }
        else if (m.mGPUTimed && frame > 0.0 && m.mCPUBusyMs >= frame * BALANCED_SHARE && m.mGPURenderMs >= frame * BALANCED_SHARE)
        {
            // The main thread hardly waits, yet the GPU's work spans nearly the whole frame too
            m.mBound = "balanced";
        }
        else
        {
            m.mBound = "cpu";
        }
        m.mMemory = m.mBiasHighFrac >= 0.5;

        const F64 fps = m.mFps;
        std::string s;
        if (m.mBound == "capped")
        {
            if (m.mSleepMs >= llmax(1.0, frame * 0.08))
            {
                s = llformat("Capped by the frame rate limit (%u fps): %.1f ms a frame (%.0f fps), %.1f ms of it spent waiting, so there is headroom to spare.",
                             m.mFPSLimit, frame, fps, m.mSleepMs);
            }
            else
            {
                s = llformat("Capped by vertical sync: %.1f ms a frame (%.0f fps), %.1f ms of it waiting for the display, so there is headroom to spare.",
                             frame, fps, m.mSwapMs);
            }
        }
        else if (m.mBound == "gpu")
        {
            s = llformat("GPU-bound: %.1f ms a frame (%.0f fps); the main thread works %.1f ms and waits %.1f ms for the graphics card.",
                         frame, fps, m.mCPUBusyMs, blocked);
        }
        else if (m.mBound == "balanced")
        {
            const bool gpu_slower = m.mGPURenderMs >= m.mCPUBusyMs;
            s = llformat("Balanced: %.1f ms a frame (%.0f fps); the main thread works %.1f ms of it and the GPU's work spans %.1f ms. "
                         "Both the main thread and the graphics card are near their limit; a change helps only if it cuts both, or the larger of the two "
                         "(here the %s, by more than %.1f ms). Saves shows what the frame gains; a graphics card and a main thread suggestion ticked together help more than either alone.",
                         frame, fps, m.mCPUBusyMs, m.mGPURenderMs, gpu_slower ? "graphics card" : "main thread",
                         std::fabs(m.mGPURenderMs - m.mCPUBusyMs));
        }
        else
        {
            s = llformat("CPU-bound: %.1f ms a frame (%.0f fps); the main thread works %.1f ms of it and waits only %.1f ms for the graphics card.",
                         frame, fps, m.mCPUBusyMs, blocked);
        }
        if (m.mBound == "balanced")
        {
            // The GPU's span is in the sentence already
        }
        else if (m.mGPUTimed)
        {
            s += llformat(" The GPU's work spans %.1f ms a frame.", m.mGPURenderMs);
        }
        else
        {
            s += " GPU times could not be measured on this system.";
        }
        if (m.mMemory)
        {
            s += llformat(" Texture memory is short: the discard bias averaged %.2f and stayed above %.1f %.0f%% of the time (%.0f of %.0f MB in use).",
                          m.mBiasAvg, BIAS_HIGH, m.mBiasHighFrac * 100.0, m.mVRAMUsedMB, m.mVRAMTargetMB);
        }
        m.mSentence = s;

        auto& v = m.mVars;
        v["$frame_ms"] = frame;
        v["$fps"] = fps;
        v["$gpu_ms"] = m.mGPURenderMs;
        v["$gpu_frame_ms"] = m.mGPUFrameMs;
        v["$gpu_timed"] = m.mGPUTimed ? 1.0 : 0.0;
        v["$cpu_busy_ms"] = m.mCPUBusyMs;
        v["$sleep_ms"] = m.mSleepMs;
        v["$swap_ms"] = m.mSwapMs;
        v["$wait_ms"] = m.mWaitMs;
        v["$headroom_ms"] = m.mHeadroomMs;
        v["$bias"] = m.mBiasAvg;
        v["$bias_max"] = m.mBiasMax;
        v["$bias_high_frac"] = m.mBiasHighFrac;
        v["$tight_frac"] = m.mTightFrac;
        v["$vram_used_mb"] = m.mVRAMUsedMB;
        v["$vram_target_mb"] = m.mVRAMTargetMB;
        v["$vram_free_frac"] = m.mVRAMFreeFrac;
        v["$avatars"] = m.mAvatars;
        v["$full_avatars"] = m.mFullAvatars;
        v["$visible_avatars"] = m.mVisibleAvatars;
        v["$particles"] = m.mParticles;
        v["$max_particles"] = (F64)m.mMaxParticles;
        v["$fps_limit"] = (F64)m.mFPSLimit;
        v["$vsync"] = m.mVSync ? 1.0 : 0.0;
        v["$amd"] = gGLManager.mIsAMD ? 1.0 : 0.0;
        v["$nvidia"] = gGLManager.mIsNVIDIA ? 1.0 : 0.0;
        v["$intel"] = gGLManager.mIsIntel ? 1.0 : 0.0;
        v["$gpu_bound"] = m.mBound == "gpu" ? 1.0 : 0.0;
        v["$cpu_bound"] = m.mBound == "cpu" ? 1.0 : 0.0;
        v["$balanced"] = m.mBound == "balanced" ? 1.0 : 0.0;
        v["$capped"] = m.mBound == "capped" ? 1.0 : 0.0;
        v["$memory_bound"] = m.mMemory ? 1.0 : 0.0;
    }

    void finishMeasure()
    {
        Measurement& m = sResult;
        m = Measurement();
        unwatchSettings();
        m.mSeconds = sTimer.getElapsedTimeF64();
        m.mFrames = sFrames;
        m.mHitches = sHitches;
        m.mHitchMaxMs = sHitchMaxMs;
        m.mFrameMs = sFrames ? sFrameSeconds * 1000.0 / sFrames : 0.0;
        m.mFps = m.mFrameMs > 0.0 ? 1000.0 / m.mFrameMs : 0.0;
        m.mSleepMs = sFrames ? sSleepMs / sFrames : 0.0;

        FSGPUTimers::getCapture(m.mGPUZones, m.mGPUFrameMs, m.mGPUFrames);
        FSGPUTimers::stopCapture();
        m.mGPUTimed = m.mGPUFrames > 0;
        for (const auto& [name, ms] : m.mGPUZones)
        {
            m.mGPU[name] = ms;
        }

        if (sCPUFrames)
        {
            for (const auto& [key, entry] : sCPU)
            {
                auto& slot = m.mCPU[entry.mName];
                slot.first += entry.mOwnMs / sCPUFrames;
                slot.second += entry.mTotalMs / sCPUFrames;
            }
        }
        sCPU.clear();

        if (sTexSamples)
        {
            m.mBiasAvg = sBiasSum / sTexSamples;
            m.mBiasMax = sBiasMax;
            m.mBiasHighFrac = (F64)sBiasHigh / sTexSamples;
            m.mTightFrac = (F64)sTight / sTexSamples;
            m.mVRAMUsedMB = sUsedSum / sTexSamples;
            m.mVRAMTargetMB = sTargetSum / sTexSamples;
            m.mVRAMFreeFrac = m.mVRAMTargetMB > 0.0 ? llmax(0.0, 1.0 - m.mVRAMUsedMB / m.mVRAMTargetMB) : 0.0;
            m.mVisibleAvatars = sVisibleSum / sTexSamples;
        }
        m.mLiveFreeMB = LLViewerTexture::sVRAMLiveFreeMegabytes;
        if (sSceneSamples)
        {
            m.mAvatars = sAvatarSum / sSceneSamples;
            m.mFullAvatars = sFullSum / sSceneSamples;
            m.mParticles = sParticleSum / sSceneSamples;
        }
        m.mMaxParticles = LLViewerPartSim::getMaxPartCount();

        static LLCachedControl<U32> max_fps(gSavedSettings, "FramePerSecondLimit", 60);
        static LLCachedControl<bool> limit_fps(gSavedSettings, "FSLimitFramerate", true);
        static LLCachedControl<bool> vsync(gSavedSettings, "RenderVSyncEnable", false);
        m.mFPSLimit = limit_fps ? (U32)max_fps : 0;
        m.mVSync = vsync;

        classify(m);
        if (m.mHitches)
        {
            m.mSentence += llformat(" %u %s over %.0f ms (the longest %.0f ms) %s left out of the averages.", m.mHitches,
                                    m.mHitches == 1 ? "frame" : "frames", HITCH_SECONDS * 1000.0, m.mHitchMaxMs,
                                    m.mHitches == 1 ? "was" : "were");
        }
        FSSettingsTuner::sMeasuring = false;
        sState = STATE_DONE;
    }

    // ---------------------------------------------------------------------------------------------
    // Rules (app_settings/fs_settings_tuner_rules.xml)

    struct Change
    {
        std::string mSetting;
        std::string mValue;     // "x2", "+1", "-1", "=0.5" or a plain value
        bool mHasMin = false;
        bool mHasMax = false;
        F64 mMin = 0.0;
        F64 mMax = 0.0;
    };

    struct Condition
    {
        std::string mName;      // a setting, or a measurement ($...)
        std::string mOp;        // eq ne gt ge lt le
        F64 mValue = 0.0;
    };

    struct Rule
    {
        std::string mId;
        std::set<std::string> mBounds;
        std::vector<std::string> mZones;   // "gpu:zone", "cpu:timer" (own time) or "cpui:timer" (inclusive), "-" before one subtracts it
        F64 mMinMs = 0.0;
        F64 mMinShare = 0.0;    // the zones' share of the frame must reach this too
        F64 mFraction = 0.0;
        std::vector<Condition> mConditions;
        std::vector<Change> mChanges;
        std::string mEffect;
        std::string mLooks;
        std::string mDescription;
    };

    std::vector<Rule> loadRules()
    {
        std::vector<Rule> rules;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, RULES_FILE);
        llifstream file(path.c_str());
        if (!file.is_open())
        {
            LL_WARNS("SettingsTuner") << "Cannot open " << path << LL_ENDL;
            return rules;
        }
        LLSD sd;
        if (LLSDSerialize::fromXMLDocument(sd, file) == LLSDParser::PARSE_FAILURE || !sd.isArray())
        {
            LL_WARNS("SettingsTuner") << path << " is not an LLSD array of rules" << LL_ENDL;
            return rules;
        }
        for (const LLSD& r : llsd::inArray(sd))
        {
            Rule rule;
            rule.mId = r["id"].asString();
            std::istringstream bounds(r["bound"].asString());
            std::string bound;
            while (std::getline(bounds, bound, ','))
            {
                LLStringUtil::trim(bound);
                if (!bound.empty())
                {
                    rule.mBounds.insert(bound);
                }
            }
            for (const LLSD& z : llsd::inArray(r["zones"]))
            {
                rule.mZones.push_back(z.asString());
            }
            rule.mMinMs = r["min_ms"].asReal();
            rule.mMinShare = r["min_share"].asReal();
            rule.mFraction = r["fraction"].asReal();
            for (const LLSD& c : llsd::inArray(r["when"]))
            {
                std::istringstream in(c.asString());
                Condition cond;
                if (in >> cond.mName >> cond.mOp >> cond.mValue)
                {
                    rule.mConditions.push_back(cond);
                }
                else
                {
                    LL_WARNS("SettingsTuner") << "Rule " << rule.mId << ": cannot read condition \"" << c.asString() << "\"" << LL_ENDL;
                    rule.mId.clear();
                }
            }
            for (const LLSD& c : llsd::inArray(r["set"]))
            {
                Change change;
                change.mSetting = c["setting"].asString();
                change.mValue = c["value"].asString();
                change.mHasMin = c.has("min");
                change.mMin = c["min"].asReal();
                change.mHasMax = c.has("max");
                change.mMax = c["max"].asReal();
                rule.mChanges.push_back(change);
            }
            rule.mEffect = r["effect"].asString();
            rule.mLooks = r["looks"].asString();
            rule.mDescription = r["description"].asString();
            if (!rule.mId.empty() && !rule.mChanges.empty() && !rule.mBounds.empty())
            {
                rules.push_back(rule);
            }
        }
        return rules;
    }

    bool testCondition(const Condition& c, const Measurement& m)
    {
        F64 value = 0.0;
        if (!c.mName.empty() && c.mName[0] == '$')
        {
            auto it = m.mVars.find(c.mName);
            if (it == m.mVars.end())
            {
                return false;
            }
            value = it->second;
        }
        else
        {
            LLControlVariablePtr control = gSavedSettings.getControl(c.mName);
            if (!control)
            {
                return false; // a setting this build doesn't have: the rule doesn't apply
            }
            value = control->getValue().asReal();
        }
        constexpr F64 eps = 1.0e-4;
        if (c.mOp == "eq") return std::fabs(value - c.mValue) < eps;
        if (c.mOp == "ne") return std::fabs(value - c.mValue) >= eps;
        if (c.mOp == "gt") return value > c.mValue + eps;
        if (c.mOp == "ge") return value >= c.mValue - eps;
        if (c.mOp == "lt") return value < c.mValue - eps;
        if (c.mOp == "le") return value <= c.mValue + eps;
        return false;
    }

    // The measured time of a rule's zones, the main thread's and the GPU's apart
    void zoneSums(const Rule& rule, const Measurement& m, F64& cpu_ms, F64& gpu_ms)
    {
        cpu_ms = gpu_ms = 0.0;
        for (const std::string& entry : rule.mZones)
        {
            // A leading '-' takes the zone off: a zone nested in another that the rule doesn't change
            const bool minus = !entry.empty() && entry[0] == '-';
            const std::string zone = minus ? entry.substr(1) : entry;
            const F64 sign = minus ? -1.0 : 1.0;
            if (zone.compare(0, 4, "gpu:") == 0)
            {
                gpu_ms += sign * gpuMs(m, zone.substr(4));
            }
            else if (zone.compare(0, 5, "cpui:") == 0)
            {
                cpu_ms += sign * cpuTotalMs(m, zone.substr(5));
            }
            else if (zone.compare(0, 4, "cpu:") == 0)
            {
                cpu_ms += sign * cpuOwnMs(m, zone.substr(4));
            }
        }
        cpu_ms = llmax(cpu_ms, 0.0);
        gpu_ms = llmax(gpu_ms, 0.0);
    }

    bool sameChanges(const std::vector<std::pair<std::string, LLSD>>& a, const std::vector<std::pair<std::string, LLSD>>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].first != b[i].first || std::fabs(a[i].second.asReal() - b[i].second.asReal()) >= 1.0e-4)
            {
                return false;
            }
        }
        return true;
    }

    // The new value of a change, in the setting's type; false when the setting doesn't exist or the
    // value can't be read
    bool newValue(const Change& change, LLControlVariable* control, LLSD& out)
    {
        const LLSD current = control->getValue();
        const F64 cur = current.asReal();
        const std::string& v = change.mValue;
        F64 val = 0.0;
        try
        {
            if (v.empty())
            {
                return false;
            }
            else if (v[0] == 'x')
            {
                val = cur * std::stod(v.substr(1));
            }
            else if (v[0] == '+' || (v[0] == '-' && v.size() > 1))
            {
                val = cur + std::stod(v);
            }
            else if (v[0] == '=')
            {
                val = std::stod(v.substr(1));
            }
            else if (v == "true" || v == "on")
            {
                val = 1.0;
            }
            else if (v == "false" || v == "off")
            {
                val = 0.0;
            }
            else
            {
                val = std::stod(v);
            }
        }
        catch (const std::exception&)
        {
            return false;
        }
        if (change.mHasMin)
        {
            val = llmax(val, change.mMin);
        }
        if (change.mHasMax)
        {
            val = llmin(val, change.mMax);
        }
        switch (control->type())
        {
        case TYPE_BOOLEAN:
            out = LLSD(val >= 0.5);
            return true;
        case TYPE_S32:
            out = LLSD((S32)std::lround(val));
            return true;
        case TYPE_U32:
            out = LLSD((S32)std::lround(llmax(val, 0.0)));
            return true;
        case TYPE_F32:
            out = LLSD(std::round(val * 1000.0) / 1000.0);
            return true;
        default:
            return false;
        }
    }

    std::string valueText(const LLSD& value, eControlType type)
    {
        if (type == TYPE_BOOLEAN)
        {
            return value.asBoolean() ? "on" : "off";
        }
        if (type == TYPE_F32)
        {
            std::string s = llformat("%.3f", value.asReal());
            while (!s.empty() && s.back() == '0')
            {
                s.pop_back();
            }
            if (!s.empty() && s.back() == '.')
            {
                s.pop_back();
            }
            return s;
        }
        return llformat("%d", value.asInteger());
    }

    bool sameValue(const LLSD& a, const LLSD& b)
    {
        return std::fabs(a.asReal() - b.asReal()) < 1.0e-4;
    }

    std::string undoPath()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, UNDO_FILE);
    }
}

// -------------------------------------------------------------------------------------------------
// Hooks

void FSSettingsTuner::frame()
{
    if (!sMeasuring)
    {
        return;
    }
    if (LLStartUp::getStartupState() < STATE_STARTED || gTeleportDisplay || LLApp::isExiting())
    {
        abortMeasure("The measurement stopped: a teleport or logout interrupted it. Press Measure again.");
        return;
    }
    if (LLPerfStats::tunables.userAutoTuneEnabled)
    {
        abortMeasure("The measurement stopped: automatic FPS tuning was switched on, and it changes settings all the "
                     "time. Switch it off and press Measure again.");
        return;
    }
    if (sRestartWanted)
    {
        // Whatever changed costs its own frames (a shader build, buffers made again) and changes
        // the scene's cost from then on: what was measured so far describes neither
        if (++sRestarts > MAX_RESTARTS)
        {
            abortMeasure("The measurement stopped: settings kept changing during it (last " + sRestartReason +
                         "). Press Measure again once they have settled.");
            return;
        }
        LL_INFOS("SettingsTuner") << "Measuring again from the start: " << sRestartReason << " changed" << LL_ENDL;
        beginMeasure(true);
        return;
    }

    const F64 t = sTimer.getElapsedTimeF64();
    if (sState == STATE_WARMUP)
    {
        if (t >= WARMUP_SECONDS)
        {
            resetSums();
            FSGPUTimers::startCapture(); // from here on
            sState = STATE_CAPTURE;
            sTimer.reset();
        }
        return;
    }
    if (sState != STATE_CAPTURE)
    {
        return;
    }
    if (t >= CAPTURE_SECONDS)
    {
        finishMeasure();
        return;
    }

    // The frame that just ended: from the last call to this one, with the limiter's sleep at its end
    const F64 dt = t - sLastFrameT;
    sLastFrameT = t;
    const S32 sleep_ms = sPendingSleepMs;
    sPendingSleepMs = 0;
    sSkipCPU = dt > HITCH_SECONDS;
    if (sSkipCPU)
    {
        ++sHitches;
        sHitchMaxMs = llmax(sHitchMaxMs, dt * 1000.0);
        return;
    }
    ++sFrames;
    sFrameSeconds += dt;
    sSleepMs += (F64)sleep_ms;
    const F64 bias = LLViewerTexture::sDesiredDiscardBias;
    sBiasSum += bias;
    sBiasMax = llmax(sBiasMax, bias);
    sBiasHigh += bias > BIAS_HIGH ? 1 : 0;
    sTight += LLViewerTexture::sFSVRAMTight ? 1 : 0;
    sUsedSum += LLViewerTexture::sVRAMUsedMegabytes;
    sTargetSum += LLViewerTexture::sVRAMTargetMegabytes;
    sVisibleSum += LLVOAvatar::sNumVisibleAvatars;
    ++sTexSamples;
    if (t >= sNextSceneSample)
    {
        sNextSceneSample = t + 0.5;
        sampleScene();
    }
}

bool FSSettingsTuner::wantCPUZones()
{
    // fs_log_profile runs after frame() and reads the block timers of the frame frame() just judged
    if (!sMeasuring || sState != STATE_CAPTURE || sSkipCPU)
    {
        return false;
    }
    ++sCPUFrames;
    return true;
}

void FSSettingsTuner::addCPUZone(const void* key, const std::string& name, F64 own_ms, F64 total_ms)
{
    CPUEntry& entry = sCPU[key];
    if (entry.mName.empty())
    {
        entry.mName = name;
    }
    entry.mOwnMs += own_ms;
    entry.mTotalMs += total_ms;
}

void FSSettingsTuner::addCapSleepMs(S32 ms)
{
    if (sState == STATE_CAPTURE)
    {
        sPendingSleepMs += ms; // counted with its frame, unless that was a hitch
    }
}

void FSSettingsTuner::restartForShaderReload()
{
    if (!sRestartWanted)
    {
        sRestartWanted = true;
        sRestartReason = "a shader reload";
    }
}

// -------------------------------------------------------------------------------------------------
// The window

FSFloaterSettingsTuner::FSFloaterSettingsTuner(const LLSD& key)
:   LLFloater(key)
{
}

FSFloaterSettingsTuner::~FSFloaterSettingsTuner()
{
    cancelMeasure();
}

bool FSFloaterSettingsTuner::postBuild()
{
    mList = getChild<LLScrollListCtrl>("suggestions");
    mSummary = getChild<LLTextBox>("summary");
    mDetails = getChild<LLTextBox>("details");
    mMeasureBtn = getChild<LLButton>("measure");
    mApplyBtn = getChild<LLButton>("apply");
    mUndoBtn = getChild<LLButton>("undo");
    mCopyBtn = getChild<LLButton>("copy_report");

    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelect(); });
    mMeasureBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { startMeasure(); });
    mApplyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onApply(); });
    mUndoBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    mCopyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCopyReport(); });
    getChild<LLButton>("close")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });

    updateButtons();
    return true;
}

void FSFloaterSettingsTuner::onOpen(const LLSD& key)
{
    if (!FSSettingsTuner::sMeasuring && !mHaveMeasurement)
    {
        startMeasure();
    }
    updateButtons();
}

void FSFloaterSettingsTuner::onClose(bool app_quitting)
{
    // Nothing is measured without the window
    if (FSSettingsTuner::sMeasuring)
    {
        cancelMeasure();
        mSummary->setText(getString("measure_cancelled"));
    }
}

void FSFloaterSettingsTuner::startMeasure()
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        mSummary->setText(getString("not_in_world"));
        return;
    }
    // Both change the draw distance and avatar settings every few seconds: every change would start the
    // measurement over, and five end it
    if (LLPerfStats::tunables.userAutoTuneEnabled)
    {
        mSummary->setText(getString("autotune_on"));
        return;
    }
    if (gSavedDrawDistance > 0.f)
    {
        mSummary->setText(getString("draw_distance_stepping"));
        return;
    }
    beginMeasure();
    mShownSeconds = -1;
    mShownRestarts = 0;
    mSuggestions.clear();
    mList->deleteAllItems();
    mDetails->setText(LLStringUtil::null);
    updateButtons();
}

void FSFloaterSettingsTuner::draw()
{
    if (FSSettingsTuner::sMeasuring)
    {
        const F64 left = sState == STATE_WARMUP ? CAPTURE_SECONDS + WARMUP_SECONDS - sTimer.getElapsedTimeF64()
                                                : CAPTURE_SECONDS - sTimer.getElapsedTimeF64();
        // setText reflows the whole text box: only when the countdown or the restart changes
        const S32 seconds = (S32)llmax(0.0, std::ceil(left));
        if (seconds != mShownSeconds || sRestarts != mShownRestarts)
        {
            mShownSeconds = seconds;
            mShownRestarts = sRestarts;
            LLStringUtil::format_map_t args;
            args["[SECONDS]"] = llformat("%d", seconds);
            std::string text = getString("measuring", args);
            if (sRestarts > 0)
            {
                LLStringUtil::format_map_t restart_args;
                restart_args["[CHANGE]"] = sRestartReason;
                text += " " + getString("measure_restarted", restart_args);
            }
            mSummary->setText(text);
        }
    }
    else if (sState == STATE_DONE)
    {
        sState = STATE_IDLE;
        onMeasured();
    }
    else if (sState == STATE_ABORTED)
    {
        sState = STATE_IDLE;
        mSummary->setText(sAbortReason);
        updateButtons();
    }
    LLFloater::draw();
}

void FSFloaterSettingsTuner::onMeasured()
{
    mHaveMeasurement = true;
    LL_INFOS("SettingsTuner") << "Measured over " << sResult.mFrames << " frames: " << sResult.mSentence << LL_ENDL;

    // After an Apply or Undo: did it help? The same place and camera make the comparison fair.
    mCompareText.clear();
    if (mHaveBaseline)
    {
        const F64 now = sResult.mFrameMs;
        const F64 gained = mBaseFrameMs - now;
        const F64 noise = llmax(COMPARE_NOISE_MS, mBaseFrameMs * 0.02);
        std::string verdict;
        if (gained >= noise)
        {
            verdict = llformat("%.1f ms faster", gained);
        }
        else if (gained <= -noise)
        {
            verdict = llformat("%.1f ms slower", -gained);
        }
        else
        {
            verdict = "no clear change";
        }
        mCompareText = llformat("After %s: was %.1f ms a frame (%.0f fps), now %.1f ms (%.0f fps), %s. Main thread work %.1f -> %.1f ms",
                                mBaseChanges.c_str(), mBaseFrameMs, mBaseFrameMs > 0.0 ? 1000.0 / mBaseFrameMs : 0.0, now, sResult.mFps,
                                verdict.c_str(), mBaseCPUMs, sResult.mCPUBusyMs);
        if (mBaseGPUTimed && sResult.mGPUTimed)
        {
            mCompareText += llformat(", GPU span %.1f -> %.1f ms", mBaseGPUMs, sResult.mGPURenderMs);
        }
        mCompareText += ".";
        LL_INFOS("SettingsTuner") << mCompareText << LL_ENDL;
        mHaveBaseline = false;
        mBaseChanges.clear();
    }
    mSummaryText = mCompareText.empty() ? sResult.mSentence : mCompareText + "\n" + sResult.mSentence;
    evaluate();
}

void FSFloaterSettingsTuner::evaluate()
{
    mSuggestions.clear();
    if (!mHaveMeasurement)
    {
        fillList();
        return;
    }
    const Measurement& m = sResult;
    // Both sides near their limit: the main thread's and the GPU's rules both apply, each cutting its side
    const bool balanced = m.mBound == "balanced";
    for (const Rule& rule : loadRules())
    {
        const bool bound_ok = rule.mBounds.count("any") || rule.mBounds.count(m.mBound) || (m.mMemory && rule.mBounds.count("memory"))
                              || (balanced && (rule.mBounds.count("cpu") || rule.mBounds.count("gpu")));
        if (!bound_ok)
        {
            continue;
        }
        F64 cpu_ms = 0.0;
        F64 gpu_ms = 0.0;
        zoneSums(rule, m, cpu_ms, gpu_ms);
        const F64 zone_ms = cpu_ms + gpu_ms;
        if (!rule.mZones.empty() && (zone_ms < rule.mMinMs || zone_ms < rule.mMinShare * m.mFrameMs))
        {
            continue;
        }
        bool ok = true;
        for (const Condition& c : rule.mConditions)
        {
            if (!testCondition(c, m))
            {
                ok = false;
                break;
            }
        }
        if (!ok)
        {
            continue;
        }

        Suggestion s;
        bool missing = false;
        for (const Change& change : rule.mChanges)
        {
            LLControlVariablePtr control = gSavedSettings.getControl(change.mSetting);
            LLSD value;
            if (!control || !newValue(change, control.get(), value))
            {
                missing = true; // a setting this build doesn't have
                break;
            }
            const LLSD current = control->getValue();
            if (sameValue(current, value))
            {
                continue;
            }
            s.mChanges.emplace_back(change.mSetting, value);
            if (!s.mChangeText.empty())
            {
                s.mChangeText += ", ";
            }
            s.mChangeText += llformat("%s %s -> %s", change.mSetting.c_str(), valueText(current, control->type()).c_str(),
                                      valueText(value, control->type()).c_str());
        }
        if (missing || s.mChanges.empty())
        {
            continue; // nothing would change
        }
        s.mRuleId = rule.mId;
        s.mSavingMs = zone_ms * rule.mFraction;
        s.mCPUSavingMs = cpu_ms * rule.mFraction;
        s.mGPUSavingMs = gpu_ms * rule.mFraction;
        s.mLooks = rule.mLooks;
        s.mDescription = rule.mDescription;
        s.mEffect = rule.mEffect;

        // The same change from another rule (its main thread and GPU sides, or its texture memory
        // side) is one row, with what each side saves
        auto same = std::find_if(mSuggestions.begin(), mSuggestions.end(),
                                 [&s](const Suggestion& o) { return sameChanges(o.mChanges, s.mChanges); });
        if (same != mSuggestions.end())
        {
            same->mRuleId += "+" + s.mRuleId;
            same->mCPUSavingMs = llmax(same->mCPUSavingMs, s.mCPUSavingMs);
            same->mGPUSavingMs = llmax(same->mGPUSavingMs, s.mGPUSavingMs);
            if (s.mSavingMs > same->mSavingMs)
            {
                same->mSavingMs = s.mSavingMs;
                same->mLooks = s.mLooks;
                same->mDescription = s.mDescription;
            }
            if (same->mEffect.empty())
            {
                same->mEffect = s.mEffect;
            }
            continue;
        }
        mSuggestions.push_back(s);
    }

    if (balanced)
    {
        // The frame waits for the slower side: what it gains is how much the slower of the two,
        // after the change, is faster than the slower of the two now
        const F64 cpu = m.mCPUBusyMs;
        const F64 gpu = m.mGPURenderMs;
        for (Suggestion& s : mSuggestions)
        {
            if (s.mCPUSavingMs <= 0.0 && s.mGPUSavingMs <= 0.0)
            {
                continue; // no estimate (texture memory, headroom)
            }
            s.mSavingMs = llmax(0.0, llmax(cpu, gpu) - llmax(cpu - s.mCPUSavingMs, gpu - s.mGPUSavingMs));
            s.mDescription += llformat(" Estimated: %.2f ms less main thread work, %.2f ms less GPU work; with both near their limit the frame gains about %.2f ms.",
                                       s.mCPUSavingMs, s.mGPUSavingMs, s.mSavingMs);
        }
    }
    std::stable_sort(mSuggestions.begin(), mSuggestions.end(),
                     [](const Suggestion& a, const Suggestion& b)
                     {
                         if (a.mSavingMs != b.mSavingMs)
                         {
                             return a.mSavingMs > b.mSavingMs;
                         }
                         return llmax(a.mCPUSavingMs, a.mGPUSavingMs) > llmax(b.mCPUSavingMs, b.mGPUSavingMs);
                     });
    fillList();
}

std::string FSFloaterSettingsTuner::savesText(const Suggestion& s) const
{
    if (s.mSavingMs >= 0.005)
    {
        return llformat("~%.2f ms", s.mSavingMs);
    }
    // Balanced: this saves time on one side only, and the other side then sets the frame
    if (s.mGPUSavingMs >= 0.005 || s.mCPUSavingMs >= 0.005)
    {
        return s.mGPUSavingMs >= s.mCPUSavingMs ? llformat("GPU %.2f ms only", s.mGPUSavingMs)
                                                : llformat("CPU %.2f ms only", s.mCPUSavingMs);
    }
    return s.mEffect;
}

void FSFloaterSettingsTuner::fillList()
{
    mList->deleteAllItems();
    mDetails->setText(LLStringUtil::null);
    for (size_t i = 0; i < mSuggestions.size(); ++i)
    {
        const Suggestion& s = mSuggestions[i];
        LLSD row;
        row["value"] = (S32)i;
        LLSD& use = row["columns"][0];
        use["column"] = "use";
        use["type"] = "checkbox";
        use["value"] = false;
        LLSD& change = row["columns"][1];
        change["column"] = "change";
        change["value"] = s.mChangeText;
        change["tool_tip"] = s.mDescription;
        LLSD& saves = row["columns"][2];
        saves["column"] = "saves";
        saves["value"] = savesText(s);
        if (s.mSavingMs < 0.005 && (s.mGPUSavingMs >= 0.005 || s.mCPUSavingMs >= 0.005))
        {
            saves["tool_tip"] = getString("one_side_only");
        }
        LLSD& looks = row["columns"][3];
        looks["column"] = "looks";
        looks["value"] = s.mLooks;
        looks["tool_tip"] = s.mLooks;
        mList->addElement(row, ADD_BOTTOM);
    }

    std::string summary = mSummaryText;
    if (mHaveMeasurement)
    {
        summary += " ";
        summary += mSuggestions.empty() ? getString("no_suggestions") : getString("pick_suggestions");
    }
    mSummary->setText(summary);
    updateButtons();
}

void FSFloaterSettingsTuner::onSelect()
{
    LLScrollListItem* item = mList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const S32 i = item->getValue().asInteger();
    if (i >= 0 && i < (S32)mSuggestions.size())
    {
        mDetails->setText(mSuggestions[i].mDescription);
    }
}

void FSFloaterSettingsTuner::onApply()
{
    // The ticked suggestions, or the selected one when none is ticked
    std::vector<S32> picked;
    for (LLScrollListItem* item : mList->getAllData())
    {
        LLScrollListCell* cell = item ? item->getColumn(0) : nullptr;
        if (cell && cell->getValue().asBoolean())
        {
            picked.push_back(item->getValue().asInteger());
        }
    }
    if (picked.empty())
    {
        if (LLScrollListItem* item = mList->getFirstSelected())
        {
            picked.push_back(item->getValue().asInteger());
        }
    }
    if (picked.empty())
    {
        mDetails->setText(getString("nothing_ticked"));
        return;
    }

    // The previous value of every setting about to change, written down first so a crash or restart
    // keeps the undo. Settings that rebuild the shaders are all set in this one go, and the viewer
    // rebuilds them once for all of them.
    // Undo puts back everything changed since the last Undo: the record of earlier Applies is kept,
    // and a setting already in it keeps the value it had before the first of them.
    LLSD previous = LLSD::emptyMap();
    {
        LLSD earlier;
        llifstream file(undoPath().c_str());
        if (file.is_open() && LLSDSerialize::fromXML(earlier, file) != LLSDParser::PARSE_FAILURE && earlier["Settings"].isMap())
        {
            previous = earlier["Settings"];
        }
    }
    std::vector<std::pair<std::string, LLSD>> changes;
    for (S32 i : picked)
    {
        if (i < 0 || i >= (S32)mSuggestions.size())
        {
            continue;
        }
        for (const auto& [name, value] : mSuggestions[i].mChanges)
        {
            LLControlVariablePtr control = gSavedSettings.getControl(name);
            if (!control)
            {
                continue;
            }
            if (!previous.has(name))
            {
                previous[name] = control->getValue();
            }
            changes.emplace_back(name, value);
        }
    }
    if (changes.empty())
    {
        return;
    }
    rememberBaseline();
    for (S32 i : picked)
    {
        if (i >= 0 && i < (S32)mSuggestions.size())
        {
            mBaseChanges += (mBaseChanges.empty() ? "" : "; ") + mSuggestions[i].mChangeText;
        }
    }
    LLSD undo;
    undo["Settings"] = previous;
    {
        llofstream file(undoPath().c_str());
        if (file.is_open())
        {
            LLSDSerialize::toPrettyXML(undo, file);
        }
        else
        {
            LL_WARNS("SettingsTuner") << "Cannot write " << undoPath() << "; Undo will not survive a restart" << LL_ENDL;
        }
    }
    // The reflection probe and SSR settings rebuild the GL buffers and the shaders at once on each
    // change: in a batch that runs once, after all of them
    fs_begin_settings_batch();
    for (const auto& [name, value] : changes)
    {
        LLControlVariablePtr control = gSavedSettings.getControl(name);
        if (control)
        {
            LL_INFOS("SettingsTuner") << "Applying " << name << " = " << value << " (was " << control->getValue() << ", Undo restores "
                                      << previous[name] << ")" << LL_ENDL;
            control->setValue(value);
        }
    }
    fs_end_settings_batch();
    // Preferences, open or not, must not put the old values back when it is closed without OK
    {
        std::vector<std::string> names;
        for (const auto& change : changes)
        {
            names.push_back(change.first);
        }
        keepInPreferences(names);
    }
    evaluate(); // what was applied no longer changes anything and drops out of the list
    mDetails->setText(getString("applied"));
}

// Preferences restores the values it was opened with when it is closed without OK, which took back what
// Apply and Undo did while it was open (the tuner is opened from its Experimental tab)
void FSFloaterSettingsTuner::keepInPreferences(const std::vector<std::string>& names)
{
    if (LLFloaterPreference* prefs = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
    {
        prefs->fsKeepCurrentValues(names);
    }
}

void FSFloaterSettingsTuner::rememberBaseline()
{
    // The measurement the first change since it was made from; later changes add to the list
    if (mHaveMeasurement && !mHaveBaseline)
    {
        mHaveBaseline = true;
        mBaseFrameMs = sResult.mFrameMs;
        mBaseCPUMs = sResult.mCPUBusyMs;
        mBaseGPUMs = sResult.mGPURenderMs;
        mBaseGPUTimed = sResult.mGPUTimed;
        mBaseChanges.clear();
    }
}

void FSFloaterSettingsTuner::onUndo()
{
    const std::string path = undoPath();
    LLSD undo;
    {
        llifstream file(path.c_str());
        if (!file.is_open() || LLSDSerialize::fromXML(undo, file) == LLSDParser::PARSE_FAILURE || !undo["Settings"].isMap())
        {
            updateButtons();
            return;
        }
    }
    rememberBaseline();
    mBaseChanges += mBaseChanges.empty() ? "Undo" : "; Undo";
    fs_begin_settings_batch();
    std::vector<std::string> undone;
    for (const auto& [name, value] : llsd::inMap(undo["Settings"]))
    {
        LLControlVariablePtr control = gSavedSettings.getControl(name);
        if (control)
        {
            LL_INFOS("SettingsTuner") << "Undo: " << name << " = " << value << LL_ENDL;
            control->setValue(value);
            undone.push_back(name);
        }
    }
    fs_end_settings_batch();
    keepInPreferences(undone);
    LLFile::remove(path);
    evaluate();
    mDetails->setText(getString("undone"));
}

void FSFloaterSettingsTuner::onCopyReport()
{
    if (!mHaveMeasurement)
    {
        return;
    }
    const Measurement& m = sResult;
    std::string report = "Firestorm settings tuner report\n";
    if (!mCompareText.empty())
    {
        report += mCompareText + "\n";
    }
    report += m.mSentence + "\n";
    report += llformat("Frames %u over %.0f s (%u over %.0f ms left out, the longest %.0f ms); frame limit %u fps, vsync %s; main thread busy %.2f, swap %.2f, waiting %.2f, limiter sleep %.2f ms a frame\n",
                       m.mFrames, m.mSeconds, m.mHitches, HITCH_SECONDS * 1000.0, m.mHitchMaxMs, m.mFPSLimit, m.mVSync ? "on" : "off", m.mCPUBusyMs, m.mSwapMs, m.mWaitMs, m.mSleepMs);
    report += llformat("Textures: bias avg %.2f max %.2f, above %.1f %.0f%% of the time, memory short %.0f%% of the time, %.0f of %.0f MB used",
                       m.mBiasAvg, m.mBiasMax, BIAS_HIGH, m.mBiasHighFrac * 100.0, m.mTightFrac * 100.0, m.mVRAMUsedMB, m.mVRAMTargetMB);
    if (m.mLiveFreeMB >= 0)
    {
        report += llformat(", %d MB free on the card", m.mLiveFreeMB);
    }
    report += "\n";
    report += llformat("Scene: %.1f avatars, %.1f fully drawn, %.1f visible; %.0f particles of %d\n", m.mAvatars, m.mFullAvatars,
                       m.mVisibleAvatars, m.mParticles, m.mMaxParticles);
    if (m.mGPUTimed)
    {
        report += llformat("GPU ms a frame over %u frames (inclusive): frame %.2f", m.mGPUFrames, m.mGPUFrameMs);
        size_t shown = 0;
        for (const auto& [name, ms] : m.mGPUZones)
        {
            if (shown++ >= 40 || ms < 0.05)
            {
                break;
            }
            report += llformat(", %s %.2f", name.c_str(), ms);
        }
        report += "\n";
    }
    std::vector<std::pair<F64, std::string>> own;
    for (const auto& [name, times] : m.mCPU)
    {
        own.emplace_back(times.first, name);
    }
    std::sort(own.begin(), own.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    report += "Main thread own ms a frame:";
    for (size_t i = 0; i < own.size() && i < 20; ++i)
    {
        report += llformat(" %s %.2f;", own[i].second.c_str(), own[i].first);
    }
    report += "\nSuggestions:\n";
    for (const Suggestion& s : mSuggestions)
    {
        report += llformat("- [%s] %s; %s; looks: %s\n", s.mRuleId.c_str(), s.mChangeText.c_str(),
                           ("saves: " + savesText(s)).c_str(), s.mLooks.c_str());
    }
    if (mSuggestions.empty())
    {
        report += "(none)\n";
    }
    const LLWString text = utf8str_to_wstring(report);
    LLClipboard::instance().copyToClipboard(text, 0, (S32)text.size());
    mDetails->setText(getString("copied"));
}

void FSFloaterSettingsTuner::updateButtons()
{
    const bool measuring = FSSettingsTuner::sMeasuring;
    mMeasureBtn->setEnabled(!measuring);
    mApplyBtn->setEnabled(!measuring && !mSuggestions.empty());
    mUndoBtn->setEnabled(!measuring && LLFile::isfile(undoPath()));
    mCopyBtn->setEnabled(!measuring && mHaveMeasurement);
}
