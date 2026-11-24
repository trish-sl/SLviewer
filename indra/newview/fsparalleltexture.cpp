/**
 * @file fsparalleltexture.cpp
 * @brief Texture decode priority face pass on worker threads (FSPerfParallelTexturePriority)
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

// The regular sweep of LLViewerTextureList::updateImagesFetchTextures visits 5% of the textures
// per frame (more under memory pressure) and calls updateImageDecodePriority for each. Most of its
// cost is the face loop: LLFace::calcPixelArea for faces not updated in the last 10 frames, then a
// biased virtual size per face reduced to a maximum. With FSPerfParallelTexturePriority on and a
// sweep of at least MIN_PARALLEL_TEXTURES eligible textures, prepare() does that loop up front:
//
//  1. (workers) for every eligible texture, collect the faces the serial loop would visit (same
//     1024 face cap per texture) whose pixel area is stale (gFrameCount - mLastTextureUpdate > 10).
//     Read only.
//  2. (main) merge the lists in sweep order and keep each face once, stamping mLastTextureUpdate as
//     the serial loop does. Faces are shared between textures (diffuse, normal and specular maps of
//     one face), so this is what makes every face belong to exactly one thread below.
//  3. (workers) calcPixelArea for the stale non-rigged faces, and mInFrustum. It writes only the
//     face's own cached fields and reads the camera (an LLSimpleton, no lock), the drawable's
//     transform, the face extents and, for particles, the spatial group bounds. ll_frand is
//     thread-local. Rigged faces stay on the main thread, which does them while the workers run:
//     their calcPixelArea may update the rigging info of an LLVolumeFace, and mesh volumes are
//     shared between objects.
//  4. (workers) the per-texture reduction, a copy of the face loop of updateImageDecodePriority
//     without the calcPixelArea call (every face it visits is fresh after 3), into a results array.
//     Settings and static state (sDesiredDiscardBias, sMaxVirtualSize, TextureScale*AreaFactor,
//     TextureCameraBoost) are read once on the main thread and passed in.
//
// Nothing here copies an LLPointer, touches GL, the fetcher or list membership: the workers get raw
// pointers and the sweep's entries vector holds the references. The main thread is inside run() for
// the whole parallel part, so no texture, face list, face or drawable changes meanwhile.
//
// The sweep then applies the results in its original order and time budget: updateImageDecodePriority
// takes the result instead of running its face loop and does the rest (max virtual size reset,
// updateSceneTextureStats, spot lights, lazy flush, processTextureStats) as before, then updateFetch.
//
// Differences from the serial sweep: faces of textures the serial loop would have stopped early on
// (max virtual size reached, or the time budget ran out before reaching the texture) get their pixel
// area refreshed too. That is a fresher value of the same cache, never an older one.
//
// FSPerfParallelTexturePriorityVerify: the sweep runs the original serial face loop (and applies its
// result) for every texture that has a parallel result and compares the two; step 3 is checked by
// recomputing a few faces per frame on the main thread.

#include "llviewerprecompiledheaders.h"

#include "fsparalleltexture.h"

#include "llappviewer.h"
#include "lldrawable.h"
#include "llface.h"
#include "llgltfmaterial.h"
#include "llspatialpartition.h"
#include "pipeline.h"
#include "llimagegl.h"
#include "llrender.h"
#include "lltextureentry.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewertexture.h"
#include "llviewertextureanim.h"
#include "llvovolume.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <xmmintrin.h>
#include <emmintrin.h>

extern bool gCubeSnapshot;

namespace
{
    constexpr U32 MIN_PARALLEL_TEXTURES = 128;  // smaller sweeps are not worth waking the workers
    constexpr U32 TEXTURES_PER_CHUNK = 16;
    constexpr U32 FACES_PER_CHUNK = 32;
    constexpr U32 MAX_FACES_TO_CHECK = 1024;    // as in updateImageDecodePriority
    constexpr U32 FACE_UPDATE_FRAMES = 10;      // as in updateImageDecodePriority
    constexpr F32 BIAS_TRS_ON_SCREEN = 1.f;     // as in updateImageDecodePriority
    constexpr F32 PIXEL_AREA_UPDATE_PERIOD = 0.1f; // as in LLFace::calcPixelArea

    //-------------------------------------------------------------------------
    // Worker pool. Persistent, started on first use, never destroyed (detached threads waiting on
    // a condition variable), so nothing has to be torn down on exit. The main thread works through
    // the same job range and then waits only for jobs already taken, so the join is bounded by the
    // longest single chunk.
    //-------------------------------------------------------------------------
    class FSTexturePool
    {
    public:
        static FSTexturePool* instance()
        {
            static FSTexturePool* sPool = new FSTexturePool();
            return sPool;
        }

        U32 getWorkerCount() const { return mWorkerCount; }

        // Runs job(0..count-1) on the workers and the caller. caller_first, if set, runs on the
        // caller after the workers are woken and before it joins in on the jobs.
        void run(U32 count, const std::function<void(U32)>& job, const std::function<void()>* caller_first = nullptr)
        {
            if (count < 2 || mWorkerCount == 0)
            {
                if (caller_first)
                {
                    (*caller_first)();
                }
                for (U32 i = 0; i < count; ++i)
                {
                    job(i);
                }
                return;
            }

            mJob = &job;
            mCount = count;
            mNext.store(0);
            mDone.store(0);
            mCSR = _mm_getcsr();   // same rounding / denormal handling as the main thread
            mOpen.store(true);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                ++mGeneration;
            }
            mCV.notify_all();

            if (caller_first)
            {
                (*caller_first)();
            }
            work();

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - join");
                waitUntil([this, count] { return mDone.load() >= count; });
            }

            // Late wakers see mOpen false and leave; wait for any that got in before.
            mOpen.store(false);
            waitUntil([this] { return mActive.load() == 0; });
            mJob = nullptr;
        }

    private:
        FSTexturePool()
        {
            const U32 hw = std::thread::hardware_concurrency();
            // the caller works too; leave cores for the fetch, decode and other viewer threads
            const U32 count = hw > 3 ? llmin(hw - 2, 3U) : 1U;
            for (U32 i = 0; i < count; ++i)
            {
                try
                {
                    std::thread(&FSTexturePool::workerLoop, this).detach();
                    ++mWorkerCount;
                }
                catch (const std::exception& e)
                {
                    LL_WARNS("Texture") << "Could not start a texture priority worker: " << e.what() << LL_ENDL;
                    break;
                }
            }
            LL_INFOS("Texture") << "Texture priority workers: " << mWorkerCount << LL_ENDL;
        }

        void work()
        {
            U32 index;
            while ((index = mNext.fetch_add(1)) < mCount)
            {
                (*mJob)(index);
                if (mDone.fetch_add(1) + 1 == mCount)
                {
                    signalDone();
                }
            }
        }

        // Spins briefly (the in-flight chunks are usually short), then blocks until a worker
        // signals mDoneCV, rather than yielding a core away for the whole wait.
        template <typename Pred> void waitUntil(Pred done)
        {
            for (U32 i = 0; i < 1024; ++i)
            {
                if (done())
                {
                    return;
                }
                _mm_pause();
            }
            std::unique_lock<std::mutex> lock(mDoneMutex);
            mDoneCV.wait(lock, done);
        }

        // after the state waitUntil tests changed: taking the mutex orders this with a waiter
        // between its test and its wait, so the wake isn't lost
        void signalDone()
        {
            {
                std::lock_guard<std::mutex> lock(mDoneMutex);
            }
            mDoneCV.notify_all();
        }

        void workerLoop()
        {
            LL_PROFILER_SET_THREAD_NAME("FS Texture Priority");
#if LL_WINDOWS
            // The main thread (HIGHEST) waits on every task taken: a worker preempted by decode or
            // other threads holds it up for a scheduler quantum. Workers only run while it waits.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
            U64 seen = 0;
            while (true)
            {
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mCV.wait(lock, [&] { return mGeneration != seen; });
                    seen = mGeneration;
                }

                // Increment before testing mOpen, the main thread clears mOpen before testing
                // mActive (both sequentially consistent): either we see it closed, or it sees us.
                mActive.fetch_add(1);
                if (mOpen.load())
                {
                    _mm_setcsr(mCSR);
                    work();
                }
                if (mActive.fetch_sub(1) == 1)
                {
                    signalDone();
                }
            }
        }

        U32 mWorkerCount = 0;

        std::mutex mMutex;
        std::condition_variable mCV;
        U64 mGeneration = 0;
        std::mutex mDoneMutex;
        std::condition_variable mDoneCV;    // mDone reached mCount, or mActive reached 0

        const std::function<void(U32)>* mJob = nullptr;
        U32 mCount = 0;
        U32 mCSR = 0;
        std::atomic<U32> mNext{ 0 };
        std::atomic<U32> mDone{ 0 };
        std::atomic<S32> mActive{ 0 };
        std::atomic<bool> mOpen{ false };
    };

    // Per-frame state, reused between frames to avoid allocations.
    bool sActive = false;
    std::vector<LLViewerFetchedTexture*> sTextures;      // entries[i], raw (the entries vector holds the references)
    std::vector<U8> sHasResult;                          // entries[i] has a result
    std::vector<FSTexturePriorityFaces> sResults;        // per entry
    std::vector<U32> sEligible;                          // entry indices that get a result
    // a cache line each: workers push_back into neighbouring chunks at the same time
    struct alignas(64) FSChunkFaces
    {
        std::vector<LLFace*> mFaces;
    };
    std::vector<FSChunkFaces> sChunkFaces;               // step 1 output, per texture chunk
    std::vector<LLFace*> sFaces;                         // step 3 input, non-rigged
    std::vector<LLFace*> sRiggedFaces;                   // step 3, main thread

    // Step 3 results kept for the verify mode
    struct FSFaceCheck
    {
        bool mThrottled;    // calcPixelArea returned its cached result
        bool mInFrustum;
        F32  mPixelArea;
        F32  mImportance;
    };
    std::vector<FSFaceCheck> sFaceChecks;

    // Step 1: the stale faces the serial face loop would visit for imagep.
    void collect_stale_faces(LLViewerFetchedTexture* imagep, U32 frame, std::vector<LLFace*>& out)
    {
        U32 face_count = 0;
        for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
        {
            face_count += imagep->getNumFaces(i);
            const S32 faces_to_check = (face_count > MAX_FACES_TO_CHECK) ? 0 : imagep->getNumFaces(i);
            for (S32 fi = 0; fi < faces_to_check; ++fi)
            {
                LLFace* face = (*(imagep->getFaceList(i)))[fi];
                if (face && face->getViewerObject() && (frame - face->mLastTextureUpdate) > FACE_UPDATE_FRAMES)
                {
                    out.push_back(face);
                }
            }
        }
    }

    bool nearly_equal(F32 a, F32 b)
    {
        return a == b || fabsf(a - b) <= 1e-5f * llmax(fabsf(a), fabsf(b));
    }

    // Verify statistics, logged every 10 seconds while verify is on.
    U32 sVerifiedTextures = 0;
    U32 sTextureMismatches = 0;
    U32 sVerifiedFaces = 0;
    U32 sFaceMismatches = 0;
    U32 sLoggedMismatches = 0;
    constexpr U32 MAX_LOGGED_MISMATCHES = 20;

    void log_verify_summary()
    {
        static LLFrameTimer timer;
        if (timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        if (sVerifiedTextures || sVerifiedFaces)
        {
            LL_INFOS("Texture") << "FSPerfParallelTexturePriorityVerify (10s): textures=" << sVerifiedTextures
                                << " texture_mismatches=" << sTextureMismatches
                                << " faces=" << sVerifiedFaces
                                << " face_mismatches=" << sFaceMismatches << LL_ENDL;
        }
        sVerifiedTextures = sTextureMismatches = sVerifiedFaces = sFaceMismatches = 0;
    }

}

//static
FSTextureFaceBias FSTextureFaceBias::current()
{
    static LLCachedControl<bool> hidden_faces(gSavedSettings, "FSTexturePriorityHiddenFaces", false);
    static LLCachedControl<F32> hidden_grace(gSavedSettings, "FSTexturePriorityHiddenGrace", 3.f);
    static LLCachedControl<bool> gltf_scale(gSavedSettings, "FSTexturePriorityGLTFScale", false);
    FSTextureFaceBias bias;
    bias.mHidden = hidden_faces && LLPipeline::sUseOcclusion > 1;
    static LLCachedControl<bool> occluded_discard(gSavedSettings, "FSTextureOccludedDiscard", false);
    bias.mOccludedDiscard = occluded_discard && LLPipeline::sUseOcclusion > 1;
    bias.mGLTFScale = gltf_scale;
    bias.mGrace = llmax((F32)hidden_grace, 0.f);
    bias.mNow = LLFrameTimer::getElapsedSeconds();
    static LLCachedControl<bool> bias_ramp(gSavedSettings, "FSTextureBiasRamp", true);
    bias.mBiasRamp = bias_ramp;
    static LLCachedControl<F32> bias_distance_scale(gSavedSettings, "FSTextureBiasDistanceScale", 8.f);
    bias.mDistanceScale = bias_ramp ? llclamp((F32)bias_distance_scale, 0.f, 64.f) : 0.f;
    static LLCachedControl<F32> camera_boost_levels(gSavedSettings, "FSTextureCameraBoostMaxLevels", 1.f);
    bias.mCameraBoostCap = camera_boost_levels >= 0.f ? exp2f(2.f * llmin((F32)camera_boost_levels, 4.f)) : 0.f;
    static LLCachedControl<bool> map_reduce(gSavedSettings, "FSTextureMaterialMapReduce", true);
    static LLCachedControl<F32> map_reduce_distance(gSavedSettings, "FSTextureMaterialMapDistance", 16.f);
    bias.mMapReduceDistance = map_reduce ? llclamp((F32)map_reduce_distance, 0.f, 256.f) : -1.f;
    static LLCachedControl<bool> avatar_small_parts(gSavedSettings, "FSTextureAvatarSmallParts", false);
    static LLCachedControl<U32> avatar_small_parts_size(gSavedSettings, "FSTextureAvatarSmallPartsSize", 128);
    if (avatar_small_parts)
    {
        const F32 side = (F32)llclamp((U32)avatar_small_parts_size, 16U, 512U);
        bias.mAvatarFloor = side * side;
    }
    return bias;
}

F32 FSTextureFaceBias::avatarFloor(const LLViewerObject* objp, F32 vsize) const
{
    if (mAvatarFloor <= vsize || !objp || !objp->isAttachment() || objp->isHUDAttachment())
    {
        return vsize; // HUDs have their own boost
    }
    return mAvatarFloor;
}

bool FSTextureFaceBias::inView(const LLFace* face) const
{
    if (!face->mInFrustum)
    {
        return false;
    }
    if (!mHidden)
    {
        return true;
    }
    // the group of the face's drawable (for a moving object, a group of its bridge's own tree)
    const LLDrawable* drawable = face->getDrawable();
    const LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
    return !(group && group->fsHiddenFor(mNow, mGrace));
}

bool FSTextureFaceBias::occluded(const LLFace* face) const
{
    if (!mOccludedDiscard || !face->mInFrustum)
    {
        return false;
    }
    const LLDrawable* drawable = face->getDrawable();
    const LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
    return group && group->fsHiddenFor(mNow, mGrace);
}

// static
F32 FSTextureFaceBias::vsizeBias(F32 max_discard, F32 desired_bias)
{
    return (F32)llroundf(powf(4, biasLevel(max_discard, desired_bias) - 1.f));
}

// static
F32 FSTextureFaceBias::biasLevel(F32 max_discard, F32 desired_bias)
{
    return llclamp(max_discard - 2.f + llclamp(desired_bias - 4.f, 0.f, 2.f), 1.f, desired_bias);
}

F32 FSTextureFaceBias::faceDivisor(bool in_view, F32 importance, F32 vsize_bias, F32 bias_level, F32 desired_bias, F32 distance,
                                   F32 max_discard) const
{
    // apply bias to offscreen faces all the time, but only to onscreen faces when bias is large
    if (!in_view)
    {
        return vsize_bias;
    }
    if (!mBiasRamp)
    {
        // use mImportanceToCamera to make bias switch a bit more gradual
        return desired_bias > 1.9f + importance / 2.f ? vsize_bias : 1.f;
    }
    // FSTextureBiasRamp: levels taken off grow with the bias, from 0 at bias 1 for the least important faces.
    // Important faces are spared the first level (up to bias 2), and less of it as the bias climbs, so at
    // bias 4 (the stock cap) every face takes the whole bias as before. The per texture floor(log4) and the
    // drop deadband then step each texture at its own point instead of the whole scene at 1.9.
    const F32 spared = importance * llclamp((4.f - desired_bias) * 0.5f, 0.f, 1.f);
    const F32 levels = llclamp(desired_bias - 1.f - spared, 0.f, bias_level - 1.f);
    F32 divisor = levels > 0.f ? exp2f(2.f * levels) : 1.f;
    // FSTextureBiasDistanceScale, from the Alchemy viewer 7.1.9 (vsize /= max(1, (bias - 1) * (1 + distance *
    // TextureBiasDistanceScale))): far faces give memory back first, what the camera is close to keeps its
    // detail. At bias 1.15 a face 50 m away loses three levels and one at 2 m none. Never below the texture's
    // own floor (as vsizeBias(): a 2048 texture stops at 256).
    if (mDistanceScale > 0.f && desired_bias > 1.f)
    {
        const F32 by_distance = (desired_bias - 1.f) * (1.f + llmax(distance, 0.f) * mDistanceScale);
        const F32 max_levels = llclamp(max_discard - 2.f, 1.f, 4.f);
        divisor = llclamp(llmax(divisor, by_distance), 1.f, exp2f(2.f * max_levels));
    }
    return divisor;
}

F32 FSTextureFaceBias::cameraBoost(F32 importance, F32 boost) const
{
    // boost resolution of textures that are important to the camera
    const F32 scale = llmax(importance * boost, 1.f);
    // FSTextureCameraBoostMaxLevels: importance is 1 for every face of an object whose bounding sphere the
    // camera is inside (a room, a building, a skybox), and x8 there raised every wall texture 1.5 levels:
    // a 2048 texture over 100k screen pixels loaded at 1024 instead of 512, four times the pixels
    return mCameraBoostCap > 0.f ? llmin(scale, mCameraBoostCap) : scale;
}

F32 FSTextureFaceBias::materialMapScale(U32 channel, const LLFace* face, const LLViewerObject* objp, bool reducible,
                                        bool occluded) const
{
    if (mMapReduceDistance < 0.f || !reducible || occluded)
    {
        return 1.f;
    }
    if (channel != LLRender::NORMAL_MAP && channel != LLRender::SPECULAR_MAP
        && channel != LLRender::METALLIC_ROUGHNESS_MAP && channel != LLRender::GLTF_NORMAL_MAP)
    {
        return 1.f;
    }
    // world volumes only: the sky uses channel 1 for its second colour texture, and HUDs keep their detail
    if (objp->getPCode() != LL_PCODE_VOLUME || objp->isHUDAttachment())
    {
        return 1.f;
    }
    return face->getFSCameraDistance() > mMapReduceDistance ? OCCLUDED_VSIZE_SCALE : 1.f;
}

F32 FSTextureFaceBias::scale(const LLViewerObject* objp, const LLTextureEntry* te, S32 te_offset, U32 channel,
                             F32 scale_min, F32 scale_max) const
{
    F32 min_scale = te ? repeats(te, channel) : 1.f;
    // texture animations that show one frame at a time screw up their own resolution
    if (te && objp->getPCode() == LL_PCODE_VOLUME)
    {
        const LLViewerTextureAnim* anim = static_cast<const LLVOVolume*>(objp)->mTextureAnimp;
        if (anim && (anim->mMode & LLTextureAnim::ON) && !(anim->mMode & (LLTextureAnim::ROTATE | LLTextureAnim::SCALE))
            && anim->mSizeX && anim->mSizeY && (anim->mFace < 0 || anim->mFace == te_offset))
        {
            min_scale = llmin(1.f / anim->mSizeX, 1.f / anim->mSizeY);
        }
    }
    return llclamp(min_scale * min_scale, scale_min, scale_max);
}

F32 FSTextureFaceBias::repeats(const LLTextureEntry* te, U32 channel) const
{
    if (mGLTFScale && channel >= LLRender::BASECOLOR_MAP && channel <= LLRender::EMISSIVE_MAP)
    {
        const LLGLTFMaterial* mat = te->getGLTFRenderMaterial();
        if (mat)
        {
            static const LLGLTFMaterial::TextureInfo info[] = {
                LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR,           // BASECOLOR_MAP
                LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS,   // METALLIC_ROUGHNESS_MAP
                LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL,               // GLTF_NORMAL_MAP
                LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE,             // EMISSIVE_MAP
            };
            const LLVector2& scale = mat->mTextureTransform[info[channel - LLRender::BASECOLOR_MAP]].mScale;
            return llmin(fabsf(scale.mV[VX]), fabsf(scale.mV[VY]));
        }
    }
    return llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT()));
}

// Inputs of the face loop read once on the main thread.
struct FSParallelTexture::Params
{
    F32 mDiscardBias;       // LLViewerTexture::sDesiredDiscardBias
    F32 mMaxVirtualSize;    // LLViewerFetchedTexture::sMaxVirtualSize
    F32 mScaleMin;          // TextureScaleMinAreaFactor
    F32 mScaleMax;          // TextureScaleMaxAreaFactor
    F32 mCameraBoost;       // TextureCameraBoost
    FSTextureFaceBias mFaceBias;
};

// Step 4: the face loop of LLViewerTextureList::updateImageDecodePriority without the
// calcPixelArea call. Keep in step with it; the verify mode compares the two.
// static
void FSParallelTexture::reduceFaces(LLViewerFetchedTexture* imagep, const Params& p, FSTexturePriorityFaces& out)
{
    F32 max_vsize = 0.f;
    bool on_screen = false;
    U32 face_count = 0;
    bool any_in_frustum = false;    // FSTextureOccludedDiscard: as in updateImageDecodePriority
    bool any_unoccluded = false;

    // get adjusted bias based on image resolution, as a vsize scaler
    LLImageGL* img = imagep->getGLTexture();
    F32 max_discard = F32(img ? img->getMaxDiscardLevel() : MAX_DISCARD_LEVEL);
    const F32 bias = FSTextureFaceBias::vsizeBias(max_discard, p.mDiscardBias);
    const F32 bias_level = FSTextureFaceBias::biasLevel(max_discard, p.mDiscardBias);
    // FSTextureMaterialMapReduce: as in updateImageDecodePriority
    const bool map_reducible = imagep->getType() == LLViewerTexture::LOD_TEXTURE
                               && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE;

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        face_count += imagep->getNumFaces(i);
        S32 faces_to_check = (face_count > MAX_FACES_TO_CHECK) ? 0 : imagep->getNumFaces(i);

        for (S32 fi = 0; fi < faces_to_check; ++fi)
        {
            LLFace* face = (*(imagep->getFaceList(i)))[fi];

            if (face && face->getViewerObject())
            {
                F32 vsize = face->getPixelArea();

                // FSTexturePriorityHiddenFaces: as in updateImageDecodePriority
                const bool in_view = p.mFaceBias.inView(face);
                on_screen |= in_view;
                // FSTextureOccludedDiscard: as in updateImageDecodePriority
                const bool occluded = p.mFaceBias.occluded(face);
                if (face->mInFrustum)
                {
                    any_in_frustum = true;
                    any_unoccluded |= !occluded;
                }

                S32 te_offset = face->getTEOffset();  // offset is -1 if not inited
                LLViewerObject* objp = face->getViewerObject();
                const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
                // FSTexturePriorityGLTFScale and texture animations: as in updateImageDecodePriority
                vsize /= p.mFaceBias.scale(objp, te, te_offset, i, p.mScaleMin, p.mScaleMax);

                // FSTextureBiasRamp, FSTextureBiasDistanceScale: as in updateImageDecodePriority
                vsize /= p.mFaceBias.faceDivisor(in_view, face->mImportanceToCamera, bias, bias_level, p.mDiscardBias,
                                                 face->getFSCameraDistance(), max_discard);
                // FSTextureMaterialMapReduce: as in updateImageDecodePriority
                vsize *= p.mFaceBias.materialMapScale(i, face, objp, map_reducible, occluded);

                if (in_view)
                {
                    // FSTextureCameraBoostMaxLevels: as in updateImageDecodePriority
                    vsize *= p.mFaceBias.cameraBoost(face->mImportanceToCamera, p.mCameraBoost);
                    vsize = p.mFaceBias.avatarFloor(objp, vsize); // FSTextureAvatarSmallParts: as in updateImageDecodePriority
                }
                if (occluded)
                {
                    vsize *= FSTextureFaceBias::OCCLUDED_VSIZE_SCALE;
                }

                max_vsize = llmax(max_vsize, vsize);

                if (max_vsize >= p.mMaxVirtualSize
                    && (on_screen || p.mDiscardBias <= BIAS_TRS_ON_SCREEN))
                {
                    break;
                }
            }
        }

        if (max_vsize >= p.mMaxVirtualSize
            && (on_screen || p.mDiscardBias <= BIAS_TRS_ON_SCREEN))
        {
            break;
        }
    }

    out.mMaxVSize = max_vsize;
    out.mOnScreen = on_screen;
    out.mFaceCount = face_count;
    out.mOnlyOccluded = any_in_frustum && !any_unoccluded;
}

// Step 3 check: recompute a few parallel calcPixelArea results on the main thread.
// static
void FSParallelTexture::verifyFaces()
{
    constexpr size_t FACES_PER_FRAME = 16;
    static size_t start = 0;
    const size_t count = sFaces.size();
    if (!count)
    {
        return;
    }
    LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - verify faces");
    start = (start + 7919) % count;
    size_t checked = 0;
    for (size_t n = 0; n < count && checked < FACES_PER_FRAME; ++n)
    {
        const size_t k = (start + n) % count;
        const FSFaceCheck& par = sFaceChecks[k];
        if (par.mThrottled)
        {
            continue;   // returned cached values; a forced recompute would legitimately differ
        }
        LLFace* face = sFaces[k];
        const F32 saved_update = face->mLastPixelAreaUpdate;
        face->mLastPixelAreaUpdate = -1.e9f;   // defeat the 0.1 s throttle
        F32 cos_angle, radius;
        const bool in_frustum = face->calcPixelArea(cos_angle, radius);
        face->mLastPixelAreaUpdate = saved_update;
        ++checked;
        ++sVerifiedFaces;
        if (in_frustum != par.mInFrustum || !nearly_equal(face->mPixelArea, par.mPixelArea)
            || !nearly_equal(face->mImportanceToCamera, par.mImportance))
        {
            ++sFaceMismatches;
            if (sLoggedMismatches < MAX_LOGGED_MISMATCHES)
            {
                ++sLoggedMismatches;
                LL_WARNS("Texture") << "FSPerfParallelTexturePriorityVerify: face pixel area mismatch"
                                    << " in_frustum " << par.mInFrustum << "/" << in_frustum
                                    << " pixel_area " << par.mPixelArea << "/" << face->mPixelArea
                                    << " importance " << par.mImportance << "/" << face->mImportanceToCamera
                                    << LL_ENDL;
            }
        }
    }
}

// static
bool FSParallelTexture::prepare(const entries_t& entries, const skip_set_t& skip)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfParallelTexturePriority", false);
    sActive = false;
    if (!enabled || gCubeSnapshot || entries.size() < MIN_PARALLEL_TEXTURES)
    {
        return false;
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - prepare");

    const size_t count = entries.size();
    sTextures.resize(count);
    sHasResult.assign(count, 0);
    sResults.resize(count);
    sEligible.clear();
    for (size_t i = 0; i < count; ++i)
    {
        LLViewerFetchedTexture* imagep = entries[i].get();
        sTextures[i] = imagep;
        // Same test as the sweep, plus the boost test that skips the face loop
        if (imagep->getNumRefs() > 1 && !skip.count(imagep) && imagep->getBoostLevel() < LLViewerFetchedTexture::BOOST_HIGH)
        {
            sEligible.push_back((U32)i);
        }
    }
    if (sEligible.size() < MIN_PARALLEL_TEXTURES)
    {
        return false;
    }

    FSTexturePool* pool = FSTexturePool::instance();
    const U32 frame = gFrameCount;
    const U32 texture_chunks = ((U32)sEligible.size() + TEXTURES_PER_CHUNK - 1) / TEXTURES_PER_CHUNK;

    // 1. stale faces, per texture chunk (workers, read only)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - stale faces");
        if (sChunkFaces.size() < texture_chunks)
        {
            sChunkFaces.resize(texture_chunks);
        }
        std::function<void(U32)> job = [frame](U32 chunk)
        {
            std::vector<LLFace*>& out = sChunkFaces[chunk].mFaces;
            out.clear();
            const U32 end = llmin((chunk + 1) * TEXTURES_PER_CHUNK, (U32)sEligible.size());
            for (U32 e = chunk * TEXTURES_PER_CHUNK; e < end; ++e)
            {
                collect_stale_faces(sTextures[sEligible[e]], frame, out);
            }
        };
        pool->run(texture_chunks, job);
    }

    // 2. each stale face once, in sweep order, stamped as the serial loop stamps it (main)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - dedupe faces");
        sFaces.clear();
        sRiggedFaces.clear();
        for (U32 chunk = 0; chunk < texture_chunks; ++chunk)
        {
            for (LLFace* face : sChunkFaces[chunk].mFaces)
            {
                if ((frame - face->mLastTextureUpdate) > FACE_UPDATE_FRAMES)
                {
                    face->mLastTextureUpdate = frame;
                    if (face->isState(LLFace::RIGGED))
                    {
                        sRiggedFaces.push_back(face);
                    }
                    else
                    {
                        sFaces.push_back(face);
                    }
                }
            }
        }
    }

    // 3. pixel areas: non-rigged faces on the workers, rigged ones on the main thread meanwhile
    const bool verify = verifying();
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - pixel areas");
        sFaceChecks.resize(sFaces.size());
        const F32 now = gFrameTimeSeconds;
        const U32 face_chunks = ((U32)sFaces.size() + FACES_PER_CHUNK - 1) / FACES_PER_CHUNK;
        std::function<void(U32)> job = [now, verify](U32 chunk)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - pixel area chunk");
            const U32 end = llmin((chunk + 1) * FACES_PER_CHUNK, (U32)sFaces.size());
            for (U32 k = chunk * FACES_PER_CHUNK; k < end; ++k)
            {
                LLFace* face = sFaces[k];
                const bool throttled = now - face->mLastPixelAreaUpdate < PIXEL_AREA_UPDATE_PERIOD;
                F32 radius;
                F32 cos_angle_to_view_dir;
                face->mInFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
                if (verify)
                {
                    sFaceChecks[k] = { throttled, face->mInFrustum, face->mPixelArea, face->mImportanceToCamera };
                }
            }
        };
        std::function<void()> rigged = []()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - rigged pixel areas");
            for (LLFace* face : sRiggedFaces)
            {
                F32 radius;
                F32 cos_angle_to_view_dir;
                face->mInFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
            }
        };
        pool->run(face_chunks, job, &rigged);
    }

    if (verify)
    {
        verifyFaces();
    }

    // 4. per-texture reduction (workers)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - reduce");
        static LLCachedControl<F32> texture_scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> texture_scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);
        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
        Params params;
        params.mDiscardBias = LLViewerTexture::sDesiredDiscardBias;
        params.mMaxVirtualSize = LLViewerFetchedTexture::sMaxVirtualSize;
        params.mScaleMin = texture_scale_min();
        params.mScaleMax = texture_scale_max();
        params.mCameraBoost = texture_camera_boost();
        params.mFaceBias = FSTextureFaceBias::current();

        std::function<void(U32)> job = [&params](U32 chunk)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("fs tex priority - reduce chunk");
            const U32 end = llmin((chunk + 1) * TEXTURES_PER_CHUNK, (U32)sEligible.size());
            for (U32 e = chunk * TEXTURES_PER_CHUNK; e < end; ++e)
            {
                const U32 i = sEligible[e];
                reduceFaces(sTextures[i], params, sResults[i]);
            }
        };
        pool->run(texture_chunks, job);
    }

    for (U32 i : sEligible)
    {
        sHasResult[i] = 1;
    }
    sActive = true;
    return true;
}

// static
const FSTexturePriorityFaces* FSParallelTexture::result(size_t index, const LLViewerFetchedTexture* imagep)
{
    if (!sActive || index >= sTextures.size() || !sHasResult[index] || sTextures[index] != imagep)
    {
        return nullptr;
    }
    return &sResults[index];
}

// static
bool FSParallelTexture::verifying()
{
    static LLCachedControl<bool> verify(gSavedSettings, "FSPerfParallelTexturePriorityVerify", false);
    return verify;
}

// static
void FSParallelTexture::verify(const LLViewerFetchedTexture* imagep, const FSTexturePriorityFaces& parallel,
                               const FSTexturePriorityFaces& serial)
{
    if (serial.mFaceCount == U32_MAX)
    {
        return; // boosted since prepare(): the serial path skipped its face loop
    }
    ++sVerifiedTextures;
    if (parallel.mOnScreen != serial.mOnScreen || parallel.mFaceCount != serial.mFaceCount
        || !nearly_equal(parallel.mMaxVSize, serial.mMaxVSize))
    {
        ++sTextureMismatches;
        if (sLoggedMismatches < MAX_LOGGED_MISMATCHES)
        {
            ++sLoggedMismatches;
            LL_WARNS("Texture") << "FSPerfParallelTexturePriorityVerify: texture " << imagep->getID()
                                << " max_vsize " << parallel.mMaxVSize << "/" << serial.mMaxVSize
                                << " on_screen " << parallel.mOnScreen << "/" << serial.mOnScreen
                                << " faces " << parallel.mFaceCount << "/" << serial.mFaceCount
                                << " (parallel/serial)" << LL_ENDL;
        }
    }
}

// static
void FSParallelTexture::finish()
{
    if (sActive && verifying())
    {
        log_verify_summary();
    }
    sActive = false;
}
