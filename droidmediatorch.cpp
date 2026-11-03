/*
 * Copyright (C) 2014-2015 Jolla Ltd.
 * Copyright (C) 2021 Open Mobile Platform LLC.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Torch control through the camera service, shared by the Camera1 and
 * Camera2 backends.
 */

#include "droidmediacamera.h"
#include <stdio.h>
#include <string>
#include <camera/Camera.h>
#include <android/log.h>
#if ANDROID_MAJOR > 5 || (ANDROID_MAJOR == 5 && ANDROID_MINOR >= 1)
#include <binder/Binder.h>
#include <binder/IInterface.h>
#include <binder/IServiceManager.h>
#if ANDROID_MAJOR < 7
#include <camera/ICameraService.h>
#else
#include <android/hardware/ICameraService.h>
#endif
#endif
#include <utils/String16.h>
#include "private.h"

#if ANDROID_MAJOR >= 15
#include <android/content/AttributionSourceState.h>
#endif

#undef LOG_TAG
#define LOG_TAG "DroidMediaTorch"

extern "C" {

static bool set_torch_mode_for_camera(const char *camera_id, bool enabled)
{
#if ANDROID_MAJOR > 5 || (ANDROID_MAJOR == 5 && ANDROID_MINOR >= 1)
    if (!camera_id) {
        ALOGE("No camera id specified");
        return false;
    }

    android::sp<android::IServiceManager> service_manager = android::defaultServiceManager();
    android::sp<android::IBinder> binder =
            service_manager->checkService(android::String16("media.camera"));
    if (binder == NULL) {
        ALOGE("Camera service is not available");
        return false;
    }

#if ANDROID_MAJOR < 7
    android::sp<android::ICameraService> camera_service =
            android::interface_cast<android::ICameraService>(binder);
#else
    android::sp<android::hardware::ICameraService> camera_service =
            android::interface_cast<android::hardware::ICameraService>(binder);
#endif
    if (camera_service == NULL) {
        ALOGE("Failed to connect to camera service");
        return false;
    }

    static android::sp<android::IBinder> client_binder = new android::BBinder();
#if ANDROID_MAJOR < 7
    android::status_t status = camera_service->setTorchMode(
            android::String16(camera_id), enabled, client_binder);
    if (status != android::NO_ERROR) {
        ALOGE("Failed to set torch mode for camera %s: %d", camera_id, status);
        return false;
    }
#else
#if ANDROID_MAJOR >= 14
#if ANDROID_MAJOR >= 15
    android::content::AttributionSourceState clientAttribution;
    clientAttribution.uid = android::hardware::ICameraService::USE_CALLING_UID;
    clientAttribution.pid = android::hardware::ICameraService::USE_CALLING_PID;
    clientAttribution.deviceId = android::kDefaultDeviceId;
    clientAttribution.packageName = "droidmedia";
#endif

    android::binder::Status status = camera_service->setTorchMode(
            std::string(camera_id), enabled, client_binder
#if ANDROID_MAJOR >= 15
            , clientAttribution, 0
#endif
            );
#else
    android::binder::Status status = camera_service->setTorchMode(
            android::String16(camera_id), enabled, client_binder);
#endif
    if (!status.isOk()) {
        ALOGE("Failed to set torch mode for camera %s: exception %d",
              camera_id, status.exceptionCode());
        return false;
    }
#endif
    return true;
#else
    (void)camera_id;
    (void)enabled;
    ALOGW("Camera service torch mode is not supported before Android 5.1");
    return false;
#endif
}

static bool set_torch_mode_by_camera_number(int camera_number, bool enabled)
{
    std::string camera_id;
    if (!droid_media_camera_get_id(camera_number, &camera_id)) {
        ALOGE("No camera id for camera %d", camera_number);
        return false;
    }
    return set_torch_mode_for_camera(camera_id.c_str(), enabled);
}

bool droid_media_camera_set_torch_mode(bool enabled)
{
    static int default_torch_camera = -1;

    if (!enabled) {
        if (default_torch_camera < 0) {
            return false;
        }

        bool success = set_torch_mode_by_camera_number(default_torch_camera, false);
        default_torch_camera = -1;
        return success;
    }

    if (default_torch_camera >= 0) {
        if (set_torch_mode_by_camera_number(default_torch_camera, true)) {
            return true;
        }
        default_torch_camera = -1;
    }

    int camera_count = droid_media_camera_get_number_of_cameras();
    bool found_back_camera = false;

    for (int i = 0; i < camera_count; ++i) {
        DroidMediaCameraInfo info;
        if (!droid_media_camera_get_info(&info, i)
                || info.facing != DROID_MEDIA_CAMERA_FACING_BACK) {
            continue;
        }

        found_back_camera = true;
        if (set_torch_mode_by_camera_number(i, true)) {
            default_torch_camera = i;
            return true;
        }
    }

    if (!found_back_camera && camera_count > 0
            && set_torch_mode_by_camera_number(0, true)) {
        default_torch_camera = 0;
        return true;
    }

    return false;
}

};
