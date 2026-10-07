/* Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Indentifier: BSD-3-Clause-Clear
 */

#undef LOG_TAG
#define LOG_TAG "SurfaceExtension-GPP"
// #define LOG_NDEBUG 0
#include "dlfcn.h"
#include <cutils/misc.h>
#include <cutils/properties.h>
#include "QtiSurfaceExtensionGPP.h"
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using ::android::IGraphicBufferProducer;
using ::android::sp;
using ::android::status_t;
using ::android::IBinder;

typedef status_t (*InitFunc_t)(sp<IGraphicBufferProducer>*, const sp<IBinder>&);

// NX809J (XDA #288/#301): GPP Init makes a SYNCHRONOUS binder call to gppservice
// (BpGPPService::createSession) from inside Surface::connect on the RenderThread. When gppservice
// wedged after a GameHub/Wine session died, SystemUI's and the launcher's RenderThreads blocked
// there forever -> whole screen frozen while the system stayed up (adb fine, no ANR recovery).
// Two guards: never attach GPP to the system UI processes (they are not games), and bound Init:
// it runs on a helper thread and a caller that waits longer than kInitTimeout gives up on GPP for
// that surface and keeps drawing; after one timeout the whole process skips GPP for kHungBackoff.
static constexpr auto kInitTimeout = std::chrono::milliseconds(800);
static constexpr auto kHungBackoff = std::chrono::seconds(60);
static std::atomic<int64_t> sGppHungUntilMs{0};

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool isNeverGppProcess() {
    std::ifstream f("/proc/self/cmdline");
    std::string name;
    std::getline(f, name, '\0');
    return name == "com.android.systemui" || name.rfind("com.android.launcher", 0) == 0 ||
            name.rfind("com.android.systemui:", 0) == 0;
}

// Runs Init with a deadline. On success *gbp is replaced like a direct Init call would.
// On timeout returns TIMED_OUT and leaves *gbp untouched; the helper thread keeps its own copies
// (and its own reference on libgppextension) so a late return touches nothing of ours.
static status_t initWithTimeout(void* initFn, sp<IGraphicBufferProducer>* gbp,
                                const sp<IBinder>& handle) {
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        status_t err = android::UNKNOWN_ERROR;
        sp<IGraphicBufferProducer> gbp;
        sp<IBinder> handle;
    };
    auto st = std::make_shared<State>();
    st->gbp = *gbp;
    st->handle = handle;
    void* lib = dlopen("libgppextension.so", RTLD_NOW | RTLD_GLOBAL);   // pin for the thread
    std::thread([st, initFn, lib]() {
        status_t err = reinterpret_cast<InitFunc_t>(initFn)(&st->gbp, st->handle);
        {
            std::lock_guard<std::mutex> l(st->m);
            st->err = err;
            st->done = true;
        }
        st->cv.notify_all();
        if (lib) dlclose(lib);
    }).detach();
    std::unique_lock<std::mutex> l(st->m);
    if (!st->cv.wait_for(l, kInitTimeout, [&] { return st->done; })) {
        sGppHungUntilMs.store(nowMs() + std::chrono::duration_cast<std::chrono::milliseconds>(
                kHungBackoff).count());
        ALOGW("gppservice did not answer createSession within %lld ms; GPP skipped for %lld s",
              (long long)kInitTimeout.count(), (long long)kHungBackoff.count());
        return android::TIMED_OUT;
    }
    if (st->err == android::OK) *gbp = st->gbp;
    return st->err;
}
typedef void (*DeinitFunc_t)(const sp<IBinder>&);
static constexpr uint32_t BQ_LAYER_COUNT = 1;

namespace android::libguiextension {
QtiSurfaceExtensionGPP::QtiSurfaceExtensionGPP(
        Surface* surface,
        const sp<IBinder> handle,
        sp<IGraphicBufferProducer>* gbp)
    : mIsEnable(false),
      mSurface(surface),
      mIsSupported(true),
      mConnectedToGpu(false),
      mUID(0),
      mClientSetBufferCount(0),
      mFrameRate(0.0f),
      mCompatibility(0),
      mChangeFrameRateStrategy(0),
      mOriginalGbp(*gbp),
      mGbp(nullptr),
      mHandle(handle),
      mLibHandler(nullptr),
      mFuncInit(nullptr),
      mFuncDeinit(nullptr),
      mAPI(0),
      mConnectedProducerListener(),
      mReportBufferRemoval(false),
      mLastQueuedBufferSlot(-1) {
    // FIRST_APPLICATION_UID / AID_APP_START is first uid for 3rd party application.
    // The system application will not enter this logic.
    mUID = getuid();
    if (mUID < FIRST_APPLICATION_UID) {
        mIsSupported = false;
        return;
    }
    if (isNeverGppProcess()) {
        mIsSupported = false;
        return;
    }

    mLibHandler = dlopen("libgppextension.so", RTLD_NOW|RTLD_GLOBAL);
    if (!mLibHandler) {
        ALOGV("%s: mHandle %p, failed dlopen libgppextension, err %s",
            __FUNCTION__, mHandle.get(), dlerror());
        mIsSupported = false;
        return;
    }
    mFuncInit = dlsym(mLibHandler, "Init");
    mFuncDeinit = dlsym(mLibHandler, "Deinit");
    if (!mFuncInit || !mFuncDeinit) {
        ALOGV("%s: mHandle %p, failed dlsym functions, err %s",
            __FUNCTION__, mHandle.get(), dlerror());
        dlclose(mLibHandler);
        mLibHandler = nullptr;
        mIsSupported = false;
        return;
    }
    ALOGV("Created Surface Extension for GPP, original buffer producer = %p", mOriginalGbp.get());
}

bool QtiSurfaceExtensionGPP::Connect(int api, sp<IGraphicBufferProducer>* gbp) {
    if (api != NATIVE_WINDOW_API_EGL) {
        ALOGV("Connect api %d is not EGL. No need to enable GPP feather.", api);
        return false;
    }

    std::lock_guard _lock{mMutex};
    mConnectedToGpu = true;
    return DynamicEnableInternal(gbp, false);
}

void QtiSurfaceExtensionGPP::Disconnect(int api, sp<IGraphicBufferProducer>* gbp) {
    if (api == NATIVE_WINDOW_API_EGL || IsGPPEnabled()) {
        std::lock_guard _lock{mMutex};
        mConnectedToGpu = false;
        DisableGPPinternal(gbp);
    }
}

bool QtiSurfaceExtensionGPP::DynamicEnable(sp<IGraphicBufferProducer>* gbp) {
    std::lock_guard _lock{mMutex};
    return DynamicEnableInternal(gbp, true);
}

void QtiSurfaceExtensionGPP::DisableGPPinternal(sp<IGraphicBufferProducer>* gbp) {
    if (mIsEnable && mFuncDeinit) {
        reinterpret_cast<DeinitFunc_t>(mFuncDeinit)(mHandle);
    }
    // NX809J: keep libgppextension loaded. QSSI dlclose()d it here but left mFuncInit/mFuncDeinit
    // pointing into the unmapped image, so a dynamic off (0x21) followed by a dynamic on (0x22)
    // on the same Surface -- RedMagic Control's per-game switch flipping while a game is alive --
    // called Init through a dangling pointer. The destructor closes the handle.
    mGbp = nullptr;
    if (mOriginalGbp != nullptr) {
        if (static_cast<uint32_t>(mFrameRate) != 0) {
            ALOGI("Change back to App set FrameRate %.1f", mFrameRate);
            mOriginalGbp->setFrameRate(mFrameRate, mCompatibility, mChangeFrameRateStrategy);
        }
        *gbp = mOriginalGbp;
        SetGraphicBufferProducer(*gbp);
        mIsEnable = false;
    } else {
        ALOGV("mOriginalGbp is not set.");
    }
}

bool QtiSurfaceExtensionGPP::DynamicEnableInternal(sp<IGraphicBufferProducer>* gbp, bool needReconnect) {
    if (mIsSupported && mConnectedToGpu) {
        char valueStr[PROPERTY_VALUE_MAX] = {0};
        property_get("vendor.gpp.frc.enable", valueStr, "0x11");//default value should not be 0x0(FRC OFF) or 0x1(FRC ON),need to other value,choose 0x11.
        int enable = -1;
        size_t pos = 0;
        int property = std::stoul(valueStr, &pos, 16);
        if (property == 0x21) {  // 0x21 dynamic off
            enable = 0;
        } else if (property == 0x22) {  // 0x22 dynamic on
            enable = 1;
        }

        ALOGV("Property enable = %d, before dynamic enable, mGraphicBufferProducer = %p ", enable, gbp->get());
        if (enable !=-1 && mIsEnable != enable) {
            if (enable) {
                ALOGV("Enabling GPP feather");
                if (nowMs() < sGppHungUntilMs.load()) {
                    ALOGV("gppservice recently unresponsive; not enabling GPP");
                    return true;
                }
                status_t err = initWithTimeout(mFuncInit, gbp, mHandle);
                if (err == OK) {
                    mIsEnable = true;
                    mIsSupported = true;
                    mGbp = *gbp;
                    if (static_cast<uint32_t>(mFrameRate) != 0 && mOriginalGbp) {
                        ALOGI("App set FrameRate %.1f, overwrite with 0.0 after GPP enabled",
                            mFrameRate);
                        mOriginalGbp->setFrameRate(0.0f, mCompatibility, mChangeFrameRateStrategy);
                    }
                } else {
                    mIsEnable = false;
                    if (err == TIMED_OUT) {
                        // Leave mIsSupported set: a later dynamic on may try again after the
                        // back-off, once gppservice has recovered.
                        return true;
                    }
                    if (err == NAME_NOT_FOUND || err == INVALID_OPERATION) {
                       mIsSupported = false;
                       ALOGV("Failed to init GPP: Surface or App is not supported by GPP");
                    } else {
                        mIsSupported = false;
                        ALOGV("Failed to init GPP: Unknown error.");
                    }
                }
            } else {
                ALOGV("Disabling GPP feather");
                DisableGPPinternal(gbp);
            }
            if (needReconnect && mIsEnable == enable && nullptr != *gbp && nullptr != mConnectedProducerListener) {
               IGraphicBufferProducer::QueueBufferOutput output;
               (*gbp)->connect(mConnectedProducerListener, mAPI, mReportBufferRemoval, &output);
               TransferBuffersToNewQueue(gbp);
            }
            if (mIsEnable == enable) {
                ALOGV("GPP dynamic On/Off succeeded, mIsEnable = %d", mIsEnable);
                return true;
            } else {
                ALOGV("GPP dynamic On/Off failed, mIsEnable = %d", mIsEnable);
                return false;
            }

        } else {
            ALOGV("No need to change BufferProducer");
            return true;
        }

        ALOGV("After dynamic enable, mGraphicBufferProducer = %p, mOriginalGbp = %p", gbp->get(), mOriginalGbp.get());
    } else {
        ALOGV("Unsupport Surface");
        return true;
    }
}

 void QtiSurfaceExtensionGPP::StoreConnect(int api, const sp<IProducerListener>& listener, bool reportBufferRemoval) {
   mAPI = api;
   mConnectedProducerListener = listener;
   mReportBufferRemoval = reportBufferRemoval;
}

 void QtiSurfaceExtensionGPP::SetGraphicBufferProducer(sp<IGraphicBufferProducer> gbp) {
    if (gbp != nullptr) {
        if (mSidebandStream.seted)
            gbp->setSidebandStream(mSidebandStream.stream);
    }
}

int QtiSurfaceExtensionGPP::query(int what, int *outValue) const {
    std::lock_guard _lock{mMutex};

    if (mOriginalGbp == nullptr) {
        ALOGE("mIsEnable = %d , mOriginalGbp must not be NULL", mIsEnable);
        return BAD_VALUE;
    }

    if (mIsEnable == true) {
        switch (what) {
            case NATIVE_WINDOW_WIDTH:
            case NATIVE_WINDOW_HEIGHT:
            case NATIVE_WINDOW_FORMAT:
            case NATIVE_WINDOW_CONSUMER_USAGE_BITS:
            case NATIVE_WINDOW_STICKY_TRANSFORM:
            case NATIVE_WINDOW_CONSUMER_RUNNING_BEHIND:
            case NATIVE_WINDOW_DEFAULT_DATASPACE:
            case NATIVE_WINDOW_BUFFER_AGE:
            case NATIVE_WINDOW_CONSUMER_IS_PROTECTED:
                // Call the mOriginalGbp directly to avoid unnecessary binder call due to those values of mGbp are the same as mOriginalGbp.
                return mOriginalGbp->query(what, outValue);
            case NATIVE_WINDOW_LAYER_COUNT:
                // All BufferQueue buffers have a single layer.
                *outValue = BQ_LAYER_COUNT;
                return NO_ERROR;
            case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
                if (mGbp != nullptr) {
                    return mGbp->query(what, outValue);
                } else {
                    ALOGW("mGbp is NULL");
                    return BAD_VALUE;
                }
            default:
                return BAD_VALUE;
        }
    } else {
        return mOriginalGbp->query(what, outValue);
    }
}

QtiSurfaceExtensionGPP::~QtiSurfaceExtensionGPP() {
    if (mIsEnable && mFuncDeinit) {
        reinterpret_cast<DeinitFunc_t>(mFuncDeinit)(mHandle);
    }
    if (mLibHandler != nullptr) {
        dlclose(mLibHandler);
    }
    mLibHandler = nullptr;
    ALOGV("~QtiSurfaceExtensionGPP()");
}

void QtiSurfaceExtensionGPP::TransferBuffersToNewQueue(sp<IGraphicBufferProducer>* gbp) {
    if (mSurface == nullptr) {
        ALOGE("NULL Surface");
        return;
    }
    int count = 0;
    for (int slot = 0; slot < (int)mSurface->mSlots.size(); ++slot) { // NX809J: A17 mSlots is a vector
        if (mSurface->mSlots[slot].buffer != nullptr) {
            count++;
        }
    }
    int minUndequeuedBuffers = 0;
    int maxDequeuedBufferCount = 0;
    if (mClientSetBufferCount == 0) {
        maxDequeuedBufferCount = 2;
    } else {
        if ((*gbp)->query(
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBuffers) == NO_ERROR) {
            maxDequeuedBufferCount = mClientSetBufferCount - minUndequeuedBuffers;
        } else {
            maxDequeuedBufferCount = 2;
        }
    }
    ALOGI("Min undequeued count %d, max dequeued count %d",
        minUndequeuedBuffers, maxDequeuedBufferCount);
    if ((count == 0) || (count != mClientSetBufferCount)) {
        ALOGI("Free buffer count %d mismatch client set buffer count %d",
            count, mClientSetBufferCount);
        (*gbp)->allowAllocation(true);
        (*gbp)->setMaxDequeuedBufferCount(maxDequeuedBufferCount);
        return;
    }
    // App should only uses existing buffers rather than dequeue any new
    // slot and request new buffers for dynamic ON/OFF case
    (*gbp)->allowAllocation(false);
    (*gbp)->setMaxDequeuedBufferCount(maxDequeuedBufferCount);
    ALOGI("DisAllow dequeue new slots and set max dequeued buffer count %d",
        maxDequeuedBufferCount);

    std::unordered_map<int, sp<GraphicBuffer>> newSlotsToBuffersMapping {};
    // TODO: Check if we need acquire lock when operate on mSlots
	int lastQueuedBufferSlot = -1;
    for (int slot = 0; slot < (int)mSurface->mSlots.size(); ++slot) { // NX809J: A17 mSlots is a vector
        if (mSurface->mSlots[slot].buffer != nullptr) {
            if (slot == mLastQueuedBufferSlot) {
                lastQueuedBufferSlot = mLastQueuedBufferSlot;
                ALOGI("mLastQueuedBufferSlot: %d", mLastQueuedBufferSlot);
                continue;
            }
            int newSlot = -1;
            status_t result = (*gbp)->attachBuffer(&newSlot, mSurface->mSlots[slot].buffer);
            if (result == NO_ERROR && newSlot != -1) {
                newSlotsToBuffersMapping.emplace(newSlot, std::move(mSurface->mSlots[slot].buffer));
                ALOGI("Same buffer %p is mapped from old slot=%d to new slot=%d",
                    &(mSurface->mSlots[slot].buffer), slot, newSlot);
                (*gbp)->cancelBuffer(newSlot, Fence::NO_FENCE);
            }
        }
    }

    if (lastQueuedBufferSlot >= 0) {
        int slot = lastQueuedBufferSlot;
        int newSlot = -1;
        status_t result = (*gbp)->attachBuffer(&newSlot, mSurface->mSlots[slot].buffer);
        if (result == NO_ERROR && newSlot != -1) {
            newSlotsToBuffersMapping.emplace(newSlot, std::move(mSurface->mSlots[slot].buffer));
            ALOGI("Same buffer %p is mapped from old slot=%d to new slot=%d",
                &(mSurface->mSlots[slot].buffer), slot, newSlot);
            (*gbp)->cancelBuffer(newSlot, Fence::NO_FENCE);
        }
    }

    for (auto& [newSlot, buffer] : newSlotsToBuffersMapping) {
        mSurface->mSlots[newSlot].buffer = std::move(buffer);
    }
    newSlotsToBuffersMapping.clear();
}

void QtiSurfaceExtensionGPP::setFrameRate(float frameRate, int8_t compatibility,
    int8_t changeFrameRateStrategy) {
    ALOGI("FrameRate %.1f, compatibility %d changeFrameRateStrategy %d",
        frameRate, compatibility, changeFrameRateStrategy);
    mFrameRate = frameRate;
    mCompatibility = compatibility;
    mChangeFrameRateStrategy = changeFrameRateStrategy;
}

} //namespace android::libguiextension