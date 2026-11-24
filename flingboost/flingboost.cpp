/*
 * Copyright (C) 2026 The PixelOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "FlingBoost"

#include <android-base/logging.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <android/binder_process.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace {

constexpr const char* kServiceName = "flingboost";
constexpr const char* kDescriptor = "android.widget.IFlingBoost";
constexpr const char* kPowerServiceName = "android.hardware.power.IPower/default";

constexpr transaction_code_t kTransactionFlingStart = FIRST_CALL_TRANSACTION;
constexpr transaction_code_t kTransactionFlingEnd = FIRST_CALL_TRANSACTION + 1;

constexpr int32_t kMinDurationMs = 50;
constexpr int32_t kMaxDurationMs = 3000;

AIBinder_Class* gFlingClass = nullptr;
std::mutex gPowerExtMutex;
AIBinder* gPowerExt = nullptr;

AIBinder* getPowerExtLocked() {
    if (gPowerExt != nullptr) {
        if (AIBinder_isAlive(gPowerExt)) {
            return gPowerExt;
        }
        AIBinder_decStrong(gPowerExt);
        gPowerExt = nullptr;
    }

    AIBinder* powerBinder = AServiceManager_checkService(kPowerServiceName);
    if (powerBinder == nullptr) {
        return nullptr;
    }

    AIBinder* extBinder = nullptr;
    binder_status_t status = AIBinder_getExtension(powerBinder, &extBinder);
    AIBinder_decStrong(powerBinder);

    if (status != STATUS_OK || extBinder == nullptr) {
        if (extBinder != nullptr) {
            AIBinder_decStrong(extBinder);
        }
        return nullptr;
    }

    if (!AIBinder_associateClass(extBinder, gFlingClass)) {
        LOG(ERROR) << "Power HAL extension descriptor mismatch";
        AIBinder_decStrong(extBinder);
        return nullptr;
    }

    gPowerExt = extBinder;
    return gPowerExt;
}

void forwardToPowerHal(transaction_code_t code, int32_t durationMs, int32_t type) {
    std::lock_guard<std::mutex> lock(gPowerExtMutex);
    AIBinder* ext = getPowerExtLocked();
    if (ext == nullptr) {
        return;
    }

    AParcel* in = nullptr;
    binder_status_t status = AIBinder_prepareTransaction(ext, &in);
    if (status != STATUS_OK || in == nullptr) {
        if (in != nullptr) AParcel_delete(in);
        return;
    }

    if (AParcel_writeInt32(in, durationMs) != STATUS_OK ||
        AParcel_writeInt32(in, type) != STATUS_OK) {
        AParcel_delete(in);
        return;
    }

    AParcel* out = nullptr;
    status = AIBinder_transact(ext, code, &in, &out, FLAG_ONEWAY);
    if (out != nullptr) {
        AParcel_delete(out);
    }
    if (status != STATUS_OK) {
        AIBinder_decStrong(gPowerExt);
        gPowerExt = nullptr;
    }
}

void* onCreate(void* args) {
    return args;
}

void onDestroy(void* /*userData*/) {}

binder_status_t onTransact(AIBinder* /*binder*/, transaction_code_t code, const AParcel* in,
                           AParcel* /*out*/) {
    int32_t durationMs = 0;
    int32_t type = 0;

    if (code == kTransactionFlingStart) {
        if (AParcel_readInt32(in, &durationMs) != STATUS_OK ||
            AParcel_readInt32(in, &type) != STATUS_OK) {
            return STATUS_BAD_VALUE;
        }
        if (durationMs <= 0) {
            return STATUS_OK;
        }
        durationMs = std::clamp(durationMs, kMinDurationMs, kMaxDurationMs);
        type = (type == 2) ? 2 : 1;
        forwardToPowerHal(kTransactionFlingStart, durationMs, type);
        return STATUS_OK;
    }

    if (code == kTransactionFlingEnd) {
        forwardToPowerHal(kTransactionFlingEnd, 0, 0);
        return STATUS_OK;
    }

    return STATUS_UNKNOWN_TRANSACTION;
}

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);

    gFlingClass = AIBinder_Class_define(kDescriptor, onCreate, onDestroy, onTransact);
    if (gFlingClass == nullptr) {
        LOG(FATAL) << "Failed to define IFlingBoost binder class";
    }

    AIBinder* binder = AIBinder_new(gFlingClass, nullptr);
    if (binder == nullptr) {
        LOG(FATAL) << "Failed to allocate IFlingBoost binder";
    }

    binder_status_t status = AServiceManager_addService(binder, kServiceName);
    if (status != STATUS_OK) {
        LOG(FATAL) << "Failed to register " << kServiceName << " service: " << status;
    }

    LOG(INFO) << "Registered " << kServiceName << " service";
    ABinderProcess_joinThreadPool();

    AIBinder_decStrong(binder);
    return 1;
}
