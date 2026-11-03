/*
 * Copyright (C) 2014-2015 Jolla Ltd.
 * Copyright (C) 2026 Jolla Mobile Ltd
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
 * Camera2 NDK implementation of the DroidMediaCamera API.
 *
 * The Camera1 style API (parameters string, preview/picture/recording
 * callbacks) is emulated on top of a capture session with up to four
 * outputs:
 *  - the preview buffer queue (zero copy, for droid_media_camera_get_buffer_queue)
 *  - a YUV_420_888 image reader, converted to NV21 for preview_frame_cb
 *  - a JPEG image reader for take_picture
 *  - a persistent encoder input surface, see droidmediarecorder2.cpp
 */

#include "droidmediacamera.h"
#include "droidmediabuffer.h"
#include "private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <android/native_window.h>
#include <media/openmax/OMX_IVCommon.h>
#include <system/camera.h>
#include <system/graphics.h>
#include <gui/Surface.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>

#undef LOG_TAG
#define LOG_TAG "DroidMediaCamera2"

namespace {

enum {
    OUTPUT_PREVIEW = 0x1,
    OUTPUT_YUV = 0x2,
    OUTPUT_JPEG = 0x4,
    OUTPUT_VIDEO = 0x8,
};

enum RequestKind {
    REQUEST_REPEATING,
    REQUEST_STILL,
    REQUEST_AF_START,
    REQUEST_AF_CANCEL,
    REQUEST_PRECAPTURE,
};

// Same number of zoom steps as Camera2Client exposes to Camera1 clients
const int ZOOM_STEPS = 100;
// Camera1 limits the preview to 1080p
const int32_t MAX_PREVIEW_WIDTH = 1920;
const int32_t MAX_PREVIEW_HEIGHT = 1080;
// Frames to wait for AF or the AE precapture sequence before giving up
const int AF_TIMEOUT_FRAMES = 150;
const int PRECAPTURE_START_FRAMES = 10;
const int PRECAPTURE_TIMEOUT_FRAMES = 60;
// Waiting for the capture sequence that fed the encoder when recording stops
const nsecs_t VIDEO_STOP_TIMEOUT_NS = 1000000000LL;
// Buffers can reach the encoder surface shortly after their capture results
const useconds_t VIDEO_STOP_GRACE_US = 60000;

struct Size {
    int32_t width;
    int32_t height;

    bool operator==(const Size &other) const {
        return width == other.width && height == other.height;
    }
    bool operator!=(const Size &other) const { return !(*this == other); }
};

struct FpsRange {
    int32_t min;
    int32_t max;
};

struct NameValue {
    const char *name;
    uint8_t value;
};

const NameValue FOCUS_MODES[] = {
    { "auto", ACAMERA_CONTROL_AF_MODE_AUTO },
    { "macro", ACAMERA_CONTROL_AF_MODE_MACRO },
    { "continuous-video", ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO },
    { "continuous-picture", ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE },
    { "edof", ACAMERA_CONTROL_AF_MODE_EDOF },
    { "infinity", ACAMERA_CONTROL_AF_MODE_OFF },
    { "fixed", ACAMERA_CONTROL_AF_MODE_OFF },
    { NULL, 0 }
};

const NameValue WHITE_BALANCE_MODES[] = {
    { "auto", ACAMERA_CONTROL_AWB_MODE_AUTO },
    { "incandescent", ACAMERA_CONTROL_AWB_MODE_INCANDESCENT },
    { "fluorescent", ACAMERA_CONTROL_AWB_MODE_FLUORESCENT },
    { "warm-fluorescent", ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT },
    { "daylight", ACAMERA_CONTROL_AWB_MODE_DAYLIGHT },
    { "cloudy-daylight", ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT },
    { "twilight", ACAMERA_CONTROL_AWB_MODE_TWILIGHT },
    { "shade", ACAMERA_CONTROL_AWB_MODE_SHADE },
    { NULL, 0 }
};

const NameValue EFFECT_MODES[] = {
    { "none", ACAMERA_CONTROL_EFFECT_MODE_OFF },
    { "mono", ACAMERA_CONTROL_EFFECT_MODE_MONO },
    { "negative", ACAMERA_CONTROL_EFFECT_MODE_NEGATIVE },
    { "solarize", ACAMERA_CONTROL_EFFECT_MODE_SOLARIZE },
    { "sepia", ACAMERA_CONTROL_EFFECT_MODE_SEPIA },
    { "posterize", ACAMERA_CONTROL_EFFECT_MODE_POSTERIZE },
    { "whiteboard", ACAMERA_CONTROL_EFFECT_MODE_WHITEBOARD },
    { "blackboard", ACAMERA_CONTROL_EFFECT_MODE_BLACKBOARD },
    { "aqua", ACAMERA_CONTROL_EFFECT_MODE_AQUA },
    { NULL, 0 }
};

const NameValue SCENE_MODES[] = {
    { "action", ACAMERA_CONTROL_SCENE_MODE_ACTION },
    { "portrait", ACAMERA_CONTROL_SCENE_MODE_PORTRAIT },
    { "landscape", ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE },
    { "night", ACAMERA_CONTROL_SCENE_MODE_NIGHT },
    { "night-portrait", ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT },
    { "theatre", ACAMERA_CONTROL_SCENE_MODE_THEATRE },
    { "beach", ACAMERA_CONTROL_SCENE_MODE_BEACH },
    { "snow", ACAMERA_CONTROL_SCENE_MODE_SNOW },
    { "sunset", ACAMERA_CONTROL_SCENE_MODE_SUNSET },
    { "steadyphoto", ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO },
    { "fireworks", ACAMERA_CONTROL_SCENE_MODE_FIREWORKS },
    { "sports", ACAMERA_CONTROL_SCENE_MODE_SPORTS },
    { "party", ACAMERA_CONTROL_SCENE_MODE_PARTY },
    { "candlelight", ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT },
    { "barcode", ACAMERA_CONTROL_SCENE_MODE_BARCODE },
    { "hdr", ACAMERA_CONTROL_SCENE_MODE_HDR },
    { NULL, 0 }
};

const NameValue ANTIBANDING_MODES[] = {
    { "off", ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF },
    { "50hz", ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ },
    { "60hz", ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ },
    { "auto", ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO },
    { NULL, 0 }
};

bool lookup(const NameValue *table, const std::string &name, uint8_t *value)
{
    for (; table->name; ++table) {
        if (name == table->name) {
            *value = table->value;
            return true;
        }
    }
    return false;
}

bool contains(const std::vector<uint8_t> &list, uint8_t value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool contains(const std::vector<Size> &list, const Size &size)
{
    return std::find(list.begin(), list.end(), size) != list.end();
}

// Comma separated names of the table entries the camera supports
std::string supported_names(const NameValue *table, const std::vector<uint8_t> &available)
{
    std::string names;
    for (; table->name; ++table) {
        if (contains(available, table->value)) {
            if (!names.empty()) {
                names += ",";
            }
            names += table->name;
        }
    }
    return names;
}

std::string size_to_string(const Size &size)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%dx%d", size.width, size.height);
    return buf;
}

std::string sizes_to_string(const std::vector<Size> &sizes)
{
    std::string str;
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (i) {
            str += ",";
        }
        str += size_to_string(sizes[i]);
    }
    return str;
}

bool parse_size(const std::string &str, Size *size)
{
    return sscanf(str.c_str(), "%dx%d", &size->width, &size->height) == 2
        && size->width > 0 && size->height > 0;
}

std::string int_to_string(int64_t value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return buf;
}

std::string float_to_string(float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", value);
    return buf;
}

bool size_area_greater(const Size &a, const Size &b)
{
    return (int64_t)a.width * a.height > (int64_t)b.width * b.height;
}

// Parses Camera1 areas, "(left,top,right,bottom,weight),..." in -1000..1000
std::vector<int32_t> parse_areas(const std::string &str)
{
    std::vector<int32_t> areas;
    const char *p = str.c_str();
    while ((p = strchr(p, '('))) {
        int32_t a[5];
        if (sscanf(p, "(%d,%d,%d,%d,%d)", &a[0], &a[1], &a[2], &a[3], &a[4]) != 5) {
            break;
        }
        // A zero weight is the Camera1 way of saying "no area"
        if (a[4] > 0 && a[0] < a[2] && a[1] < a[3]) {
            areas.insert(areas.end(), a, a + 5);
        }
        ++p;
    }
    return areas;
}

android::Mutex g_manager_lock;
ACameraManager *g_manager = NULL;

ACameraManager *camera_manager()
{
    android::Mutex::Autolock locker(g_manager_lock);
    if (!g_manager) {
        g_manager = ACameraManager_create();
    }
    return g_manager;
}

bool camera_id_less(const std::string &a, const std::string &b)
{
    // Camera ids are usually numbers, keep "10" after "2"
    char *end_a, *end_b;
    long num_a = strtol(a.c_str(), &end_a, 10);
    long num_b = strtol(b.c_str(), &end_b, 10);
    bool numeric_a = !a.empty() && *end_a == '\0';
    bool numeric_b = !b.empty() && *end_b == '\0';
    if (numeric_a && numeric_b) {
        return num_a < num_b;
    }
    if (numeric_a != numeric_b) {
        return numeric_a;
    }
    return a < b;
}

std::vector<std::string> camera_ids()
{
    std::vector<std::string> ids;
    ACameraManager *manager = camera_manager();
    ACameraIdList *list = NULL;

    if (!manager || ACameraManager_getCameraIdList(manager, &list) != ACAMERA_OK || !list) {
        ALOGE("Failed to get the camera id list");
        return ids;
    }

    for (int i = 0; i < list->numCameras; ++i) {
        ids.push_back(list->cameraIds[i]);
    }
    ACameraManager_deleteCameraIdList(list);

    std::sort(ids.begin(), ids.end(), camera_id_less);
    return ids;
}

ACameraMetadata *camera_characteristics(int camera_number, std::string *id)
{
    std::vector<std::string> ids = camera_ids();
    if (camera_number < 0 || camera_number >= (int)ids.size()) {
        ALOGE("No camera %d", camera_number);
        return NULL;
    }

    ACameraMetadata *chars = NULL;
    if (ACameraManager_getCameraCharacteristics(camera_manager(), ids[camera_number].c_str(),
                                                &chars) != ACAMERA_OK) {
        ALOGE("Failed to get characteristics of camera %s", ids[camera_number].c_str());
        return NULL;
    }

    if (id) {
        *id = ids[camera_number];
    }
    return chars;
}

std::vector<uint8_t> metadata_u8(const ACameraMetadata *metadata, uint32_t tag)
{
    ACameraMetadata_const_entry entry;
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK) {
        return std::vector<uint8_t>();
    }
    return std::vector<uint8_t>(entry.data.u8, entry.data.u8 + entry.count);
}

std::vector<int32_t> metadata_i32(const ACameraMetadata *metadata, uint32_t tag)
{
    ACameraMetadata_const_entry entry;
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK) {
        return std::vector<int32_t>();
    }
    return std::vector<int32_t>(entry.data.i32, entry.data.i32 + entry.count);
}

std::vector<float> metadata_float(const ACameraMetadata *metadata, uint32_t tag)
{
    ACameraMetadata_const_entry entry;
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK) {
        return std::vector<float>();
    }
    return std::vector<float>(entry.data.f, entry.data.f + entry.count);
}

bool metadata_first_u8(const ACameraMetadata *metadata, uint32_t tag, uint8_t *value)
{
    ACameraMetadata_const_entry entry;
    if (ACameraMetadata_getConstEntry(metadata, tag, &entry) != ACAMERA_OK || entry.count < 1) {
        return false;
    }
    *value = entry.data.u8[0];
    return true;
}

std::vector<Size> output_sizes(const ACameraMetadata *chars, int32_t format)
{
    std::vector<Size> sizes;
    std::vector<int32_t> configs = metadata_i32(chars, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);

    for (size_t i = 0; i + 3 < configs.size(); i += 4) {
        if (configs[i] == format
                && configs[i + 3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
            Size size = { configs[i + 1], configs[i + 2] };
            if (!contains(sizes, size)) {
                sizes.push_back(size);
            }
        }
    }

    std::sort(sizes.begin(), sizes.end(), size_area_greater);
    return sizes;
}

} // namespace

extern "C" {

struct _DroidMediaCameraRecordingData
{
    // Raw recording frames are not supported by the Camera2 backend
    nsecs_t ts;
};

struct _DroidMediaCamera
{
    _DroidMediaCamera();

    bool open(int camera_number);
    void close();

    void initStaticInfo();
    void initParameters();
    void setStatic(const char *key, const std::string &value);
    void setDefault(const char *key, const std::string &value);
    std::string param(const char *key) const;
    int paramInt(const char *key, int fallback) const;
    bool setParameters(const char *params);
    char *flattenParameters() const;

    Size previewSize() const;
    Size pictureSize() const;
    bool previewCallbacksEnabled() const;
    int wantedOutputs() const;
    bool ensureReaders();
    bool ensureSession();
    bool createSession(int outputs);
    void destroySession();
    ACaptureRequest *buildRequest(int targets, RequestKind kind);
    void applySettings(ACaptureRequest *request, int targets, RequestKind kind);
    void visibleRect(int32_t rect[4]) const;
    bool updateRepeating();
    void deleteDeadReaders();

    bool startPreview();
    void stopPreview();
    bool takePicture(int msgType);
    bool startAutoFocus();
    bool cancelAutoFocus();
    void setPreviewCallbackFlags(int flags);
    bool enableFaceDetection(bool enable);

    bool startVideo();
    void stopVideo();
    bool configureVideo(int32_t width, int32_t height);
    ANativeWindow *videoSurface();

    void submitStill(ACameraCaptureSession *session, ACaptureRequest *request);
    void processResult(ACameraCaptureSession *session, const ACameraMetadata *result,
                       bool afTrigger, bool precaptureTrigger);
    void sendFaces(const ACameraMetadata *result);
    void yuvImageAvailable(AImageReader *reader);
    void jpegImageAvailable(AImageReader *reader);
    void deviceError(int error);

    // Device, session and outputs. Guarded by m_lock, which is only taken by API calls.
    android::Mutex m_lock;
    std::string m_id;
    ACameraMetadata *m_chars;
    ACameraDevice *m_device;
    ACameraCaptureSession *m_session;
    ACaptureSessionOutputContainer *m_container;
    std::vector<ACaptureSessionOutput *> m_sessionOutputs;
    int m_sessionOutputFlags;
    // What the session was created for, it may have fewer outputs
    int m_sessionWantedOutputs;
    Size m_sessionPreviewSize;
    Size m_sessionPictureSize;
    Size m_sessionVideoSize;

    android::sp<DroidMediaBufferQueue> m_queue;
    android::sp<android::Surface> m_previewWindow;
    ACameraOutputTarget *m_previewTarget;

    AImageReader *m_yuvReader;
    ANativeWindow *m_yuvWindow;
    ACameraOutputTarget *m_yuvTarget;
    Size m_yuvSize;

    AImageReader *m_jpegReader;
    ANativeWindow *m_jpegWindow;
    ACameraOutputTarget *m_jpegTarget;
    Size m_jpegSize;

    ANativeWindow *m_videoWindow;
    ACameraOutputTarget *m_videoTarget;
    Size m_videoSize;
    bool m_videoActive;
    bool m_videoRecording;

    // Readers replaced while m_lock is held. Their listeners may call back
    // into the API, so they are deleted after m_lock is released.
    std::vector<AImageReader *> m_deadReaders;

    bool m_previewRunning;
    bool m_faceDetection;

    // Camera1 parameters
    std::map<std::string, std::string> m_params;
    std::set<std::string> m_readOnly;

    // Static camera information
    std::vector<Size> m_previewSizes;
    std::vector<Size> m_videoSizes;
    std::vector<Size> m_pictureSizes;
    std::vector<Size> m_thumbnailSizes;
    std::vector<FpsRange> m_fpsRanges;
    std::vector<uint8_t> m_afModes;
    std::vector<uint8_t> m_aeModes;
    std::vector<uint8_t> m_awbModes;
    std::vector<uint8_t> m_effectModes;
    std::vector<uint8_t> m_sceneModes;
    std::vector<uint8_t> m_antibandingModes;
    std::vector<uint8_t> m_faceDetectModes;
    std::vector<uint8_t> m_stabilizationModes;
    std::vector<int32_t> m_requestKeys;
    bool m_flashAvailable;
    bool m_aeLockAvailable;
    bool m_awbLockAvailable;
    float m_minFocusDistance;
    int32_t m_activeArray[4];
    float m_maxZoom;
    bool m_zoomRatio;
    int32_t m_compensationRange[2];
    float m_compensationStep;
    int32_t m_maxRegions[3];
    int32_t m_maxFaces;

    // State shared with the NDK callback threads. Guarded by m_stateLock, which
    // is never held while calling into the NDK or the client.
    android::Mutex m_stateLock;
    android::Condition m_sequenceCond;
    int m_repeatingSequence;
    int m_finishedSequence;
    int m_repeatingTargets;
    uint8_t m_afMode;
    bool m_afPending;
    bool m_afTriggerSeen;
    int m_afFrames;
    bool m_focusMoveEnabled;
    bool m_focusMoving;
    bool m_precapturePending;
    bool m_precaptureSeen;
    bool m_precaptureStarted;
    int m_precaptureFrames;
    ACaptureRequest *m_pendingStill;
    bool m_stillPending;
    int m_stillMsgType;
    bool m_facesEnabled;
    size_t m_lastFaceCount;
    int32_t m_faceRect[4];
    bool m_closing;

    // Written by the API thread, read by the image reader threads
    volatile int m_previewCallbackFlags;
    std::vector<uint8_t> m_nv21;

    DroidMediaCameraCallbacks m_cb;
    void *m_cb_data;

    ACameraDevice_StateCallbacks m_deviceCallbacks;
    ACameraCaptureSession_stateCallbacks m_sessionCallbacks;
    ACameraCaptureSession_captureCallbacks m_repeatingCallbacks;
    ACameraCaptureSession_captureCallbacks m_afCallbacks;
    ACameraCaptureSession_captureCallbacks m_precaptureCallbacks;
    ACameraCaptureSession_captureCallbacks m_stillCallbacks;
    AImageReader_ImageListener m_yuvListener;
    AImageReader_ImageListener m_jpegListener;
};

}

namespace {

// Releases the API lock and then deletes the image readers replaced under it
class ApiLock {
public:
    ApiLock(DroidMediaCamera *camera) : m_camera(camera) { m_camera->m_lock.lock(); }
    ~ApiLock() { m_camera->deleteDeadReaders(); }
private:
    DroidMediaCamera *m_camera;
};

void device_disconnected(void *context, ACameraDevice *device)
{
    static_cast<DroidMediaCamera *>(context)->deviceError(-1);
}

void device_error(void *context, ACameraDevice *device, int error)
{
    static_cast<DroidMediaCamera *>(context)->deviceError(error);
}

void session_closed(void *context, ACameraCaptureSession *session)
{
    ALOGD("Capture session %p closed", session);
}

void session_ready(void *context, ACameraCaptureSession *session)
{
}

void session_active(void *context, ACameraCaptureSession *session)
{
}

void repeating_completed(void *context, ACameraCaptureSession *session,
                         ACaptureRequest *request, const ACameraMetadata *result)
{
    static_cast<DroidMediaCamera *>(context)->processResult(session, result, false, false);
}

void repeating_sequence_finished(void *context, int sequence)
{
    DroidMediaCamera *camera = static_cast<DroidMediaCamera *>(context);
    android::Mutex::Autolock locker(camera->m_stateLock);
    camera->m_finishedSequence = std::max(camera->m_finishedSequence, sequence);
    camera->m_sequenceCond.broadcast();
}

void repeating_sequence_completed(void *context, ACameraCaptureSession *session,
                                  int sequence, int64_t frame_number)
{
    repeating_sequence_finished(context, sequence);
}

void repeating_sequence_aborted(void *context, ACameraCaptureSession *session, int sequence)
{
    repeating_sequence_finished(context, sequence);
}

void af_completed(void *context, ACameraCaptureSession *session,
                  ACaptureRequest *request, const ACameraMetadata *result)
{
    static_cast<DroidMediaCamera *>(context)->processResult(session, result, true, false);
}

void af_failed(void *context, ACameraCaptureSession *session,
               ACaptureRequest *request, ACameraCaptureFailure *failure)
{
    DroidMediaCamera *camera = static_cast<DroidMediaCamera *>(context);
    bool report;
    {
        android::Mutex::Autolock locker(camera->m_stateLock);
        report = camera->m_afPending;
        camera->m_afPending = false;
    }

    ALOGW("Autofocus trigger failed, reason %d", failure->reason);
    if (report && camera->m_cb.focus_cb) {
        camera->m_cb.focus_cb(camera->m_cb_data, 0);
    }
}

void precapture_completed(void *context, ACameraCaptureSession *session,
                          ACaptureRequest *request, const ACameraMetadata *result)
{
    static_cast<DroidMediaCamera *>(context)->processResult(session, result, false, true);
}

void precapture_failed(void *context, ACameraCaptureSession *session,
                       ACaptureRequest *request, ACameraCaptureFailure *failure)
{
    DroidMediaCamera *camera = static_cast<DroidMediaCamera *>(context);
    ACaptureRequest *still;
    {
        android::Mutex::Autolock locker(camera->m_stateLock);
        still = camera->m_pendingStill;
        camera->m_pendingStill = NULL;
        camera->m_precapturePending = false;
    }

    // Take the picture anyway, possibly without a converged flash exposure
    ALOGW("Precapture trigger failed, reason %d", failure->reason);
    if (still) {
        camera->submitStill(session, still);
    }
}

void still_started(void *context, ACameraCaptureSession *session,
                   const ACaptureRequest *request, int64_t timestamp)
{
    DroidMediaCamera *camera = static_cast<DroidMediaCamera *>(context);
    int msgType;
    {
        android::Mutex::Autolock locker(camera->m_stateLock);
        msgType = camera->m_stillMsgType;
    }

    if ((msgType & CAMERA_MSG_SHUTTER) && camera->m_cb.shutter_cb) {
        camera->m_cb.shutter_cb(camera->m_cb_data);
    }
}

void still_failed(void *context, ACameraCaptureSession *session,
                  ACaptureRequest *request, ACameraCaptureFailure *failure)
{
    DroidMediaCamera *camera = static_cast<DroidMediaCamera *>(context);

    if (failure->wasImageCaptured) {
        // The JPEG is still delivered
        return;
    }

    bool closing;
    {
        android::Mutex::Autolock locker(camera->m_stateLock);
        camera->m_stillPending = false;
        closing = camera->m_closing;
    }

    ALOGE("Still capture failed, reason %d", failure->reason);

    // The client waits for exactly one compressed image per picture. Flushed
    // captures belong to a preview that is being stopped, anything else means
    // that the picture will never arrive.
    if (!closing && failure->reason != CAPTURE_FAILURE_REASON_FLUSHED && camera->m_cb.error_cb) {
        camera->m_cb.error_cb(camera->m_cb_data, CAMERA_ERROR_UNKNOWN);
    }
}

void yuv_image_available(void *context, AImageReader *reader)
{
    static_cast<DroidMediaCamera *>(context)->yuvImageAvailable(reader);
}

void jpeg_image_available(void *context, AImageReader *reader)
{
    static_cast<DroidMediaCamera *>(context)->jpegImageAvailable(reader);
}

// Packs a YUV_420_888 image into NV21 (Y plane followed by interleaved V/U).
// The plane layout is whatever the HAL produced: use each plane's row and
// pixel stride and do not assume planar or semi-planar chroma or its order.
bool image_to_nv21(AImage *image, std::vector<uint8_t> &out)
{
    int32_t width = 0, height = 0, format = 0, planes = 0;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    AImage_getFormat(image, &format);
    AImage_getNumberOfPlanes(image, &planes);

    if (format != AIMAGE_FORMAT_YUV_420_888 || planes < 3 || width < 2 || height < 2) {
        ALOGE("Unexpected preview image format 0x%x with %d planes", format, planes);
        return false;
    }

    uint8_t *data[3];
    int length[3];
    int32_t row_stride[3];
    int32_t pixel_stride[3];
    for (int i = 0; i < 3; ++i) {
        if (AImage_getPlaneData(image, i, &data[i], &length[i]) != AMEDIA_OK
                || AImage_getPlaneRowStride(image, i, &row_stride[i]) != AMEDIA_OK
                || AImage_getPlanePixelStride(image, i, &pixel_stride[i]) != AMEDIA_OK) {
            ALOGE("Failed to get plane %d of the preview image", i);
            return false;
        }
    }

    const int32_t chroma_width = width / 2;
    const int32_t chroma_height = height / 2;

    // The last row of a plane may end right after its last sample
    if ((int64_t)row_stride[0] * (height - 1) + (int64_t)pixel_stride[0] * (width - 1) >= length[0]) {
        ALOGE("Preview luma plane is too small");
        return false;
    }
    for (int i = 1; i < 3; ++i) {
        if ((int64_t)row_stride[i] * (chroma_height - 1)
                + (int64_t)pixel_stride[i] * (chroma_width - 1) >= length[i]) {
            ALOGE("Preview chroma plane %d is too small", i);
            return false;
        }
    }

    out.resize((size_t)width * height + (size_t)chroma_width * chroma_height * 2);
    uint8_t *dst = out.data();

    for (int32_t y = 0; y < height; ++y) {
        const uint8_t *src = data[0] + (size_t)y * row_stride[0];
        if (pixel_stride[0] == 1) {
            memcpy(dst, src, width);
        } else {
            for (int32_t x = 0; x < width; ++x) {
                dst[x] = src[x * pixel_stride[0]];
            }
        }
        dst += width;
    }

    // Plane 1 is Cb (U) and plane 2 is Cr (V). When V is already interleaved
    // right before U the rows are NV21 as they are.
    const uint8_t *u = data[1];
    const uint8_t *v = data[2];
    bool nv21 = pixel_stride[1] == 2 && pixel_stride[2] == 2 && u == v + 1
        && row_stride[1] == row_stride[2];

    for (int32_t y = 0; y < chroma_height; ++y) {
        const uint8_t *u_row = u + (size_t)y * row_stride[1];
        const uint8_t *v_row = v + (size_t)y * row_stride[2];
        if (nv21) {
            memcpy(dst, v_row, chroma_width * 2);
        } else {
            for (int32_t x = 0; x < chroma_width; ++x) {
                dst[2 * x] = v_row[x * pixel_stride[2]];
                dst[2 * x + 1] = u_row[x * pixel_stride[1]];
            }
        }
        dst += chroma_width * 2;
    }

    return true;
}

int32_t normalized_to_sensor(int32_t value, int32_t start, int32_t length)
{
    return start + (int32_t)(((int64_t)value + 1000) * length / 2000);
}

int32_t sensor_to_normalized(int32_t value, int32_t start, int32_t length)
{
    if (length <= 0) {
        return 0;
    }
    int32_t normalized = (int32_t)(((int64_t)value - start) * 2000 / length) - 1000;
    return std::min(1000, std::max(-1000, normalized));
}

} // namespace

_DroidMediaCamera::_DroidMediaCamera() :
    m_chars(NULL),
    m_device(NULL),
    m_session(NULL),
    m_container(NULL),
    m_sessionOutputFlags(0),
    m_sessionWantedOutputs(0),
    m_previewTarget(NULL),
    m_yuvReader(NULL),
    m_yuvWindow(NULL),
    m_yuvTarget(NULL),
    m_jpegReader(NULL),
    m_jpegWindow(NULL),
    m_jpegTarget(NULL),
    m_videoWindow(NULL),
    m_videoTarget(NULL),
    m_videoActive(false),
    m_videoRecording(false),
    m_previewRunning(false),
    m_faceDetection(false),
    m_flashAvailable(false),
    m_aeLockAvailable(false),
    m_awbLockAvailable(false),
    m_minFocusDistance(0),
    m_maxZoom(1),
    m_zoomRatio(false),
    m_compensationStep(0),
    m_maxFaces(0),
    m_repeatingSequence(-1),
    m_finishedSequence(-1),
    m_repeatingTargets(0),
    m_afMode(ACAMERA_CONTROL_AF_MODE_OFF),
    m_afPending(false),
    m_afTriggerSeen(false),
    m_afFrames(0),
    m_focusMoveEnabled(false),
    m_focusMoving(false),
    m_precapturePending(false),
    m_precaptureSeen(false),
    m_precaptureStarted(false),
    m_precaptureFrames(0),
    m_pendingStill(NULL),
    m_stillPending(false),
    m_stillMsgType(0),
    m_facesEnabled(false),
    m_lastFaceCount(0),
    m_closing(false),
    m_previewCallbackFlags(CAMERA_FRAME_CALLBACK_FLAG_NOOP),
    m_cb_data(0)
{
    memset(&m_cb, 0x0, sizeof(m_cb));
    memset(m_activeArray, 0x0, sizeof(m_activeArray));
    memset(m_compensationRange, 0x0, sizeof(m_compensationRange));
    memset(m_maxRegions, 0x0, sizeof(m_maxRegions));
    memset(m_faceRect, 0x0, sizeof(m_faceRect));
    m_sessionPreviewSize = m_sessionPictureSize = m_sessionVideoSize = m_yuvSize = m_jpegSize = m_videoSize = Size{0, 0};

    m_deviceCallbacks.context = this;
    m_deviceCallbacks.onDisconnected = device_disconnected;
    m_deviceCallbacks.onError = device_error;

    m_sessionCallbacks.context = this;
    m_sessionCallbacks.onClosed = session_closed;
    m_sessionCallbacks.onReady = session_ready;
    m_sessionCallbacks.onActive = session_active;

    // Requests can't carry a user context before API 28 and the request in a
    // callback is a copy, so each kind of request gets its own callbacks.
    memset(&m_repeatingCallbacks, 0x0, sizeof(m_repeatingCallbacks));
    m_repeatingCallbacks.context = this;
    m_repeatingCallbacks.onCaptureCompleted = repeating_completed;
    m_repeatingCallbacks.onCaptureSequenceCompleted = repeating_sequence_completed;
    m_repeatingCallbacks.onCaptureSequenceAborted = repeating_sequence_aborted;

    memset(&m_afCallbacks, 0x0, sizeof(m_afCallbacks));
    m_afCallbacks.context = this;
    m_afCallbacks.onCaptureCompleted = af_completed;
    m_afCallbacks.onCaptureFailed = af_failed;

    memset(&m_precaptureCallbacks, 0x0, sizeof(m_precaptureCallbacks));
    m_precaptureCallbacks.context = this;
    m_precaptureCallbacks.onCaptureCompleted = precapture_completed;
    m_precaptureCallbacks.onCaptureFailed = precapture_failed;

    memset(&m_stillCallbacks, 0x0, sizeof(m_stillCallbacks));
    m_stillCallbacks.context = this;
    m_stillCallbacks.onCaptureStarted = still_started;
    m_stillCallbacks.onCaptureFailed = still_failed;

    m_yuvListener.context = this;
    m_yuvListener.onImageAvailable = yuv_image_available;
    m_jpegListener.context = this;
    m_jpegListener.onImageAvailable = jpeg_image_available;
}

bool _DroidMediaCamera::open(int camera_number)
{
    m_chars = camera_characteristics(camera_number, &m_id);
    if (!m_chars) {
        return false;
    }

    initStaticInfo();
    initParameters();

    m_queue = new DroidMediaBufferQueue("DroidMediaCameraBufferQueue");
    if (!m_queue->connectListener()) {
        ALOGE("Failed to connect buffer queue listener");
        return false;
    }
    // A preview stream, like the one Camera1 sets up. The size is set from
    // preview-size when the session is created.
    m_queue->setDefaultBufferFormat(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);
    m_previewWindow = static_cast<android::Surface *>(m_queue->window());
    if (ACameraOutputTarget_create(m_previewWindow.get(), &m_previewTarget) != ACAMERA_OK) {
        ALOGE("Failed to create the preview output target");
        return false;
    }

    camera_status_t status = ACameraManager_openCamera(camera_manager(), m_id.c_str(),
                                                       &m_deviceCallbacks, &m_device);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to open camera %s: %d", m_id.c_str(), status);
        m_device = NULL;
        return false;
    }

    ALOGI("Opened camera %s", m_id.c_str());
    return true;
}

void _DroidMediaCamera::close()
{
    std::vector<AImageReader *> readers;

    {
        android::Mutex::Autolock locker(m_lock);
        {
            android::Mutex::Autolock state(m_stateLock);
            m_closing = true;
            if (m_pendingStill) {
                ACaptureRequest_free(m_pendingStill);
                m_pendingStill = NULL;
            }
        }

        destroySession();

        if (m_device) {
            ACameraDevice_close(m_device);
            m_device = NULL;
        }

        if (m_yuvReader) {
            readers.push_back(m_yuvReader);
        }
        if (m_jpegReader) {
            readers.push_back(m_jpegReader);
        }
        readers.insert(readers.end(), m_deadReaders.begin(), m_deadReaders.end());
        m_deadReaders.clear();
        m_yuvReader = m_jpegReader = NULL;
    }

    // The listeners may be waiting for m_lock in a client callback
    for (size_t i = 0; i < readers.size(); ++i) {
        AImageReader_delete(readers[i]);
    }

    if (m_previewTarget) {
        ACameraOutputTarget_free(m_previewTarget);
        m_previewTarget = NULL;
    }
    if (m_yuvTarget) {
        ACameraOutputTarget_free(m_yuvTarget);
        m_yuvTarget = NULL;
    }
    if (m_jpegTarget) {
        ACameraOutputTarget_free(m_jpegTarget);
        m_jpegTarget = NULL;
    }
    if (m_videoTarget) {
        ACameraOutputTarget_free(m_videoTarget);
        m_videoTarget = NULL;
    }
    if (m_videoWindow) {
        ANativeWindow_release(m_videoWindow);
        m_videoWindow = NULL;
    }

    if (m_queue.get()) {
        m_queue->setCallbacks(0, 0);
    }
    m_previewWindow.clear();

    if (m_chars) {
        ACameraMetadata_free(m_chars);
        m_chars = NULL;
    }
}

void _DroidMediaCamera::initStaticInfo()
{
    std::vector<Size> priv = output_sizes(m_chars, AIMAGE_FORMAT_PRIVATE);
    std::vector<Size> yuv = output_sizes(m_chars, AIMAGE_FORMAT_YUV_420_888);

    // The preview goes to both a private and, for callbacks, a YUV stream
    for (size_t i = 0; i < priv.size(); ++i) {
        if (priv[i].width <= MAX_PREVIEW_WIDTH && priv[i].height <= MAX_PREVIEW_HEIGHT
                && contains(yuv, priv[i])) {
            m_previewSizes.push_back(priv[i]);
        }
    }
    m_videoSizes = priv;
    m_pictureSizes = output_sizes(m_chars, AIMAGE_FORMAT_JPEG);

    std::vector<int32_t> thumbnails = metadata_i32(m_chars, ACAMERA_JPEG_AVAILABLE_THUMBNAIL_SIZES);
    for (size_t i = 0; i + 1 < thumbnails.size(); i += 2) {
        Size size = { thumbnails[i], thumbnails[i + 1] };
        m_thumbnailSizes.push_back(size);
    }

    std::vector<int32_t> fps = metadata_i32(m_chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
    for (size_t i = 0; i + 1 < fps.size(); i += 2) {
        FpsRange range = { fps[i], fps[i + 1] };
        m_fpsRanges.push_back(range);
    }

    m_afModes = metadata_u8(m_chars, ACAMERA_CONTROL_AF_AVAILABLE_MODES);
    m_aeModes = metadata_u8(m_chars, ACAMERA_CONTROL_AE_AVAILABLE_MODES);
    m_awbModes = metadata_u8(m_chars, ACAMERA_CONTROL_AWB_AVAILABLE_MODES);
    m_effectModes = metadata_u8(m_chars, ACAMERA_CONTROL_AVAILABLE_EFFECTS);
    m_sceneModes = metadata_u8(m_chars, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES);
    m_antibandingModes = metadata_u8(m_chars, ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES);
    m_faceDetectModes = metadata_u8(m_chars, ACAMERA_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES);
    m_stabilizationModes = metadata_u8(m_chars, ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES);
    m_requestKeys = metadata_i32(m_chars, ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS);

    uint8_t value;
    m_flashAvailable = metadata_first_u8(m_chars, ACAMERA_FLASH_INFO_AVAILABLE, &value)
        && value == ACAMERA_FLASH_INFO_AVAILABLE_TRUE;
    m_aeLockAvailable = metadata_first_u8(m_chars, ACAMERA_CONTROL_AE_LOCK_AVAILABLE, &value)
        && value == ACAMERA_CONTROL_AE_LOCK_AVAILABLE_TRUE;
    m_awbLockAvailable = metadata_first_u8(m_chars, ACAMERA_CONTROL_AWB_LOCK_AVAILABLE, &value)
        && value == ACAMERA_CONTROL_AWB_LOCK_AVAILABLE_TRUE;

    std::vector<float> distance = metadata_float(m_chars, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE);
    m_minFocusDistance = distance.empty() ? 0 : distance[0];

    std::vector<int32_t> active = metadata_i32(m_chars, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE);
    if (active.size() >= 4) {
        // left, top, width, height
        memcpy(m_activeArray, active.data(), sizeof(m_activeArray));
    }

    std::vector<float> zoom = metadata_float(m_chars, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM);
    m_maxZoom = zoom.empty() ? 1 : zoom[0];
#if ANDROID_MAJOR >= 11
    std::vector<float> ratio = metadata_float(m_chars, ACAMERA_CONTROL_ZOOM_RATIO_RANGE);
    if (ratio.size() >= 2 && ratio[1] > 1
            && std::find(m_requestKeys.begin(), m_requestKeys.end(),
                         (int32_t)ACAMERA_CONTROL_ZOOM_RATIO) != m_requestKeys.end()) {
        // Zooming out of the default field of view isn't possible with Camera1
        m_zoomRatio = true;
        m_maxZoom = ratio[1];
    }
#endif
    if (m_maxZoom < 1) {
        m_maxZoom = 1;
    }

    std::vector<int32_t> compensation = metadata_i32(m_chars, ACAMERA_CONTROL_AE_COMPENSATION_RANGE);
    if (compensation.size() >= 2) {
        m_compensationRange[0] = compensation[0];
        m_compensationRange[1] = compensation[1];
    }
    ACameraMetadata_const_entry entry;
    if (ACameraMetadata_getConstEntry(m_chars, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &entry) == ACAMERA_OK
            && entry.count > 0 && entry.data.r[0].denominator) {
        m_compensationStep = (float)entry.data.r[0].numerator / entry.data.r[0].denominator;
    }

    std::vector<int32_t> regions = metadata_i32(m_chars, ACAMERA_CONTROL_MAX_REGIONS);
    if (regions.size() >= 3) {
        // AE, AWB, AF
        memcpy(m_maxRegions, regions.data(), sizeof(m_maxRegions));
    }

    std::vector<int32_t> faces = metadata_i32(m_chars, ACAMERA_STATISTICS_INFO_MAX_FACE_COUNT);
    m_maxFaces = faces.empty() ? 0 : faces[0];
}

void _DroidMediaCamera::setStatic(const char *key, const std::string &value)
{
    m_params[key] = value;
    m_readOnly.insert(key);
}

void _DroidMediaCamera::setDefault(const char *key, const std::string &value)
{
    m_params[key] = value;
}

std::string _DroidMediaCamera::param(const char *key) const
{
    std::map<std::string, std::string>::const_iterator it = m_params.find(key);
    return it == m_params.end() ? std::string() : it->second;
}

int _DroidMediaCamera::paramInt(const char *key, int fallback) const
{
    std::string value = param(key);
    if (value.empty()) {
        return fallback;
    }
    // Some clients write integer parameters as floats
    return (int)strtol(value.c_str(), NULL, 10);
}

void _DroidMediaCamera::initParameters()
{
    // Sizes
    setStatic("preview-size-values", sizes_to_string(m_previewSizes));
    setStatic("video-size-values", sizes_to_string(m_videoSizes));
    setStatic("picture-size-values", sizes_to_string(m_pictureSizes));

    Size picture = m_pictureSizes.empty() ? Size{640, 480} : m_pictureSizes[0];
    Size preview = m_previewSizes.empty() ? Size{640, 480} : m_previewSizes[0];
    // The largest preview with the aspect ratio of the largest picture
    for (size_t i = 0; i < m_previewSizes.size(); ++i) {
        if ((int64_t)m_previewSizes[i].width * picture.height
                == (int64_t)m_previewSizes[i].height * picture.width) {
            preview = m_previewSizes[i];
            break;
        }
    }
    Size video = preview;
    Size full_hd = { 1920, 1080 };
    if (contains(m_videoSizes, full_hd)) {
        video = full_hd;
    }

    setDefault("preview-size", size_to_string(preview));
    setDefault("picture-size", size_to_string(picture));
    setDefault("video-size", size_to_string(video));
    setStatic("preferred-preview-size-for-video", size_to_string(preview));

    // Formats. preview_frame_cb data is always NV21.
    setStatic("preview-format-values", "yuv420sp");
    setDefault("preview-format", "yuv420sp");
    setStatic("picture-format-values", "jpeg");
    setDefault("picture-format", "jpeg");
    setStatic("video-frame-format", "yuv420sp");

    // Frame rates, in fps * 1000 like Camera1
    std::string ranges;
    std::set<int32_t> rates;
    int32_t frame_rate = 0;
    for (size_t i = 0; i < m_fpsRanges.size(); ++i) {
        if (!ranges.empty()) {
            ranges += ",";
        }
        ranges += "(" + int_to_string(m_fpsRanges[i].min * 1000) + ","
            + int_to_string(m_fpsRanges[i].max * 1000) + ")";
        rates.insert(m_fpsRanges[i].max);
        if (m_fpsRanges[i].max <= 30) {
            frame_rate = std::max(frame_rate, m_fpsRanges[i].max);
        }
    }
    if (!frame_rate) {
        frame_rate = rates.empty() ? 30 : *rates.begin();
    }
    // The widest range ending at the default frame rate
    FpsRange preview_range = { frame_rate, frame_rate };
    for (size_t i = 0; i < m_fpsRanges.size(); ++i) {
        if (m_fpsRanges[i].max == frame_rate && m_fpsRanges[i].min < preview_range.min) {
            preview_range = m_fpsRanges[i];
        }
    }
    std::string rate_values;
    for (std::set<int32_t>::const_iterator it = rates.begin(); it != rates.end(); ++it) {
        if (!rate_values.empty()) {
            rate_values += ",";
        }
        rate_values += int_to_string(*it);
    }
    setStatic("preview-fps-range-values", ranges);
    setDefault("preview-fps-range", int_to_string(preview_range.min * 1000) + ","
               + int_to_string(preview_range.max * 1000));
    setStatic("preview-frame-rate-values", rate_values);
    setDefault("preview-frame-rate", int_to_string(frame_rate));

    // JPEG
    setDefault("jpeg-quality", "95");
    setDefault("jpeg-thumbnail-quality", "90");
    Size thumbnail = { 0, 0 };
    for (size_t i = 0; i < m_thumbnailSizes.size(); ++i) {
        if (size_area_greater(m_thumbnailSizes[i], thumbnail)) {
            thumbnail = m_thumbnailSizes[i];
        }
    }
    setStatic("jpeg-thumbnail-size-values", sizes_to_string(m_thumbnailSizes));
    setDefault("jpeg-thumbnail-width", int_to_string(thumbnail.width));
    setDefault("jpeg-thumbnail-height", int_to_string(thumbnail.height));
    setDefault("rotation", "0");

    // Focus
    std::string focus_modes = supported_names(FOCUS_MODES, m_afModes);
    // AF_MODE_OFF is "fixed" for fixed focus lenses and "infinity" otherwise
    std::string::size_type pos;
    if ((pos = focus_modes.find("infinity,fixed")) != std::string::npos) {
        focus_modes.replace(pos, strlen("infinity,fixed"),
                            m_minFocusDistance > 0 ? "infinity" : "fixed");
    }
    setStatic("focus-mode-values", focus_modes);
    if (contains(m_afModes, ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE)) {
        setDefault("focus-mode", "continuous-picture");
    } else if (contains(m_afModes, ACAMERA_CONTROL_AF_MODE_AUTO)) {
        setDefault("focus-mode", "auto");
    } else {
        setDefault("focus-mode", m_minFocusDistance > 0 ? "infinity" : "fixed");
    }
    setStatic("max-num-focus-areas", int_to_string(m_maxRegions[2]));
    setStatic("max-num-metering-areas", int_to_string(m_maxRegions[0]));
    setDefault("focus-areas", "(0,0,0,0,0)");
    setDefault("metering-areas", "(0,0,0,0,0)");

    // Flash
    std::string flash_modes = "off";
    if (m_flashAvailable) {
        if (contains(m_aeModes, ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH)) {
            flash_modes += ",auto";
        }
        if (contains(m_aeModes, ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH)) {
            flash_modes += ",on";
        }
        if (contains(m_aeModes, ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE)) {
            flash_modes += ",red-eye";
        }
        flash_modes += ",torch";
    }
    setStatic("flash-mode-values", flash_modes);
    setDefault("flash-mode", flash_modes.find("auto") != std::string::npos ? "auto" : "off");

    // 3A modes
    setStatic("whitebalance-values", supported_names(WHITE_BALANCE_MODES, m_awbModes));
    setDefault("whitebalance", "auto");
    setStatic("effect-values", supported_names(EFFECT_MODES, m_effectModes));
    setDefault("effect", "none");
    std::string scene_modes = supported_names(SCENE_MODES, m_sceneModes);
    setStatic("scene-mode-values", scene_modes.empty() ? "auto" : "auto," + scene_modes);
    setDefault("scene-mode", "auto");
    setStatic("antibanding-values", supported_names(ANTIBANDING_MODES, m_antibandingModes));
    setDefault("antibanding", contains(m_antibandingModes, ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO)
               ? "auto" : "off");

    // Exposure
    setStatic("exposure-compensation-step", float_to_string(m_compensationStep));
    setStatic("min-exposure-compensation", int_to_string(m_compensationRange[0]));
    setStatic("max-exposure-compensation", int_to_string(m_compensationRange[1]));
    setDefault("exposure-compensation", "0");
    setStatic("auto-exposure-lock-supported", m_aeLockAvailable ? "true" : "false");
    setDefault("auto-exposure-lock", "false");
    setStatic("auto-whitebalance-lock-supported", m_awbLockAvailable ? "true" : "false");
    setDefault("auto-whitebalance-lock", "false");

    // Zoom, in Camera1 steps with ratios in percent
    std::string ratios;
    for (int i = 0; i < ZOOM_STEPS; ++i) {
        if (i) {
            ratios += ",";
        }
        ratios += int_to_string((int)(100 * (1 + i * (m_maxZoom - 1) / (ZOOM_STEPS - 1)) + 0.5f));
    }
    setStatic("zoom-supported", m_maxZoom > 1 ? "true" : "false");
    setStatic("smooth-zoom-supported", "false");
    setStatic("max-zoom", int_to_string(ZOOM_STEPS - 1));
    setStatic("zoom-ratios", ratios);
    setDefault("zoom", "0");

    // Lens
    std::vector<float> focal = metadata_float(m_chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
    std::vector<float> physical = metadata_float(m_chars, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE);
    if (!focal.empty() && focal[0] > 0 && physical.size() >= 2) {
        setStatic("focal-length", float_to_string(focal[0]));
        setStatic("horizontal-view-angle",
                  float_to_string(2 * atanf(physical[0] / (2 * focal[0])) * 180 / M_PI));
        setStatic("vertical-view-angle",
                  float_to_string(2 * atanf(physical[1] / (2 * focal[0])) * 180 / M_PI));
    }

    // Faces
    bool faces = contains(m_faceDetectModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE)
        || contains(m_faceDetectModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL);
    setStatic("max-num-detected-faces-hw", int_to_string(faces ? m_maxFaces : 0));
    setStatic("max-num-detected-faces-sw", "0");

    // Video
    setStatic("video-stabilization-supported",
              contains(m_stabilizationModes, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON)
              ? "true" : "false");
    setDefault("video-stabilization", "false");
    setStatic("video-snapshot-supported", "false");
    setDefault("recording-hint", "false");
}

bool _DroidMediaCamera::setParameters(const char *params)
{
    std::map<std::string, std::string> values = m_params;
    std::set<std::string> given;
    std::string str(params ? params : "");
    std::string::size_type start = 0;

    while (start < str.size()) {
        std::string::size_type end = str.find(';', start);
        if (end == std::string::npos) {
            end = str.size();
        }
        std::string item = str.substr(start, end - start);
        start = end + 1;

        std::string::size_type eq = item.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        std::string key = item.substr(0, eq);
        given.insert(key);
        if (!m_readOnly.count(key)) {
            values[key] = item.substr(eq + 1);
        }
    }

    // Camera1 replaces all parameters, removing GPS data is done by leaving it out
    for (std::map<std::string, std::string>::iterator it = values.begin(); it != values.end();) {
        if (it->first.compare(0, 4, "gps-") == 0 && !given.count(it->first)) {
            values.erase(it++);
        } else {
            ++it;
        }
    }

    Size size;
    if (!parse_size(values["preview-size"], &size) || !contains(m_previewSizes, size)) {
        ALOGE("Unsupported preview-size %s", values["preview-size"].c_str());
        return false;
    }
    if (!parse_size(values["picture-size"], &size) || !contains(m_pictureSizes, size)) {
        ALOGE("Unsupported picture-size %s", values["picture-size"].c_str());
        return false;
    }
    if (!values["video-size"].empty()
            && (!parse_size(values["video-size"], &size) || !contains(m_videoSizes, size))) {
        ALOGE("Unsupported video-size %s", values["video-size"].c_str());
        return false;
    }

    int min_fps, max_fps;
    if (sscanf(values["preview-fps-range"].c_str(), "%d,%d", &min_fps, &max_fps) == 2) {
        bool found = false;
        for (size_t i = 0; i < m_fpsRanges.size(); ++i) {
            if (m_fpsRanges[i].min * 1000 == min_fps && m_fpsRanges[i].max * 1000 == max_fps) {
                found = true;
                break;
            }
        }
        if (!found) {
            ALOGE("Unsupported preview-fps-range %s", values["preview-fps-range"].c_str());
            return false;
        }
    }

    m_params = values;

    // Stream sizes are applied when the preview is (re)started or a picture is taken
    return updateRepeating();
}

char *_DroidMediaCamera::flattenParameters() const
{
    std::string str;
    for (std::map<std::string, std::string>::const_iterator it = m_params.begin();
         it != m_params.end(); ++it) {
        if (!str.empty()) {
            str += ";";
        }
        str += it->first + "=" + it->second;
    }

    char *params = (char *)malloc(str.size() + 1);
    if (!params) {
        ALOGE("Failed to allocate enough memory for camera parameters");
        return NULL;
    }
    memcpy(params, str.c_str(), str.size() + 1);
    return params;
}

Size _DroidMediaCamera::previewSize() const
{
    Size size = { 640, 480 };
    parse_size(param("preview-size"), &size);
    return size;
}

Size _DroidMediaCamera::pictureSize() const
{
    Size size = { 640, 480 };
    parse_size(param("picture-size"), &size);
    return size;
}

bool _DroidMediaCamera::previewCallbacksEnabled() const
{
    return m_previewCallbackFlags & CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK;
}

int _DroidMediaCamera::wantedOutputs() const
{
    int outputs = OUTPUT_PREVIEW;

    // Video is recorded with the viewfinder and the encoder, the pair every
    // camera supports. Still capture outputs are left out then.
    if (m_videoActive && m_videoWindow) {
        outputs |= OUTPUT_VIDEO;
        if (previewCallbacksEnabled()) {
            outputs |= OUTPUT_YUV;
        }
    } else {
        outputs |= OUTPUT_JPEG | OUTPUT_YUV;
    }

    return outputs;
}

bool _DroidMediaCamera::ensureReaders()
{
    Size preview = previewSize();
    Size picture = pictureSize();

    if (m_yuvReader && m_yuvSize != preview) {
        m_deadReaders.push_back(m_yuvReader);
        ACameraOutputTarget_free(m_yuvTarget);
        m_yuvReader = NULL;
        m_yuvWindow = NULL;
        m_yuvTarget = NULL;
    }
    if (!m_yuvReader) {
        if (AImageReader_new(preview.width, preview.height, AIMAGE_FORMAT_YUV_420_888, 4,
                             &m_yuvReader) != AMEDIA_OK
                || AImageReader_setImageListener(m_yuvReader, &m_yuvListener) != AMEDIA_OK
                || AImageReader_getWindow(m_yuvReader, &m_yuvWindow) != AMEDIA_OK
                || ACameraOutputTarget_create(m_yuvWindow, &m_yuvTarget) != ACAMERA_OK) {
            ALOGE("Failed to create the preview callback image reader");
            if (m_yuvReader) {
                m_deadReaders.push_back(m_yuvReader);
            }
            m_yuvReader = NULL;
            m_yuvWindow = NULL;
            return false;
        }
        m_yuvSize = preview;
    }

    if (m_jpegReader && m_jpegSize != picture) {
        m_deadReaders.push_back(m_jpegReader);
        ACameraOutputTarget_free(m_jpegTarget);
        m_jpegReader = NULL;
        m_jpegWindow = NULL;
        m_jpegTarget = NULL;
    }
    if (!m_jpegReader) {
        if (AImageReader_new(picture.width, picture.height, AIMAGE_FORMAT_JPEG, 2,
                             &m_jpegReader) != AMEDIA_OK
                || AImageReader_setImageListener(m_jpegReader, &m_jpegListener) != AMEDIA_OK
                || AImageReader_getWindow(m_jpegReader, &m_jpegWindow) != AMEDIA_OK
                || ACameraOutputTarget_create(m_jpegWindow, &m_jpegTarget) != ACAMERA_OK) {
            ALOGE("Failed to create the JPEG image reader");
            if (m_jpegReader) {
                m_deadReaders.push_back(m_jpegReader);
            }
            m_jpegReader = NULL;
            m_jpegWindow = NULL;
            return false;
        }
        m_jpegSize = picture;
    }

    return true;
}

bool _DroidMediaCamera::ensureSession()
{
    Size preview = previewSize();
    int outputs = wantedOutputs();

    if (!ensureReaders()) {
        return false;
    }

    // A combination the HAL refused before is not retried until something changes
    if (m_session && m_sessionPreviewSize == preview
            && (m_sessionOutputFlags == outputs || m_sessionWantedOutputs == outputs)
            && (!(m_sessionOutputFlags & OUTPUT_JPEG) || m_sessionPictureSize == m_jpegSize)
            && (!(m_sessionOutputFlags & OUTPUT_VIDEO) || m_sessionVideoSize == m_videoSize)) {
        return true;
    }

    // A buffer queue consumer reports this as its size to the camera
    m_queue->setDefaultBufferSize(preview.width, preview.height);

    // Not every HAL takes a second YUV stream or JPEG next to the others
    bool created = createSession(outputs);
    if (!created && (outputs & OUTPUT_YUV)) {
        created = createSession(outputs & ~OUTPUT_YUV);
        if (created) {
            ALOGW("Preview callbacks are not available with this stream combination");
        }
    }
    if (!created && (outputs & OUTPUT_JPEG)) {
        created = createSession(outputs & ~(OUTPUT_YUV | OUTPUT_JPEG));
        if (created) {
            ALOGW("Still capture is not available with this stream combination");
        }
    }

    if (created) {
        m_sessionWantedOutputs = outputs;
    }
    return created;
}

bool _DroidMediaCamera::createSession(int outputs)
{
    ACaptureSessionOutputContainer *container = NULL;
    std::vector<ACaptureSessionOutput *> session_outputs;
    ACameraCaptureSession *session = NULL;
    camera_status_t status;

    if (ACaptureSessionOutputContainer_create(&container) != ACAMERA_OK) {
        ALOGE("Failed to create a session output container");
        return false;
    }

    const struct {
        int flag;
        ANativeWindow *window;
    } windows[] = {
        { OUTPUT_PREVIEW, m_previewWindow.get() },
        { OUTPUT_YUV, m_yuvWindow },
        { OUTPUT_JPEG, m_jpegWindow },
        { OUTPUT_VIDEO, m_videoWindow },
    };

    for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); ++i) {
        if (!(outputs & windows[i].flag)) {
            continue;
        }
        ACaptureSessionOutput *output = NULL;
        if (!windows[i].window
                || ACaptureSessionOutput_create(windows[i].window, &output) != ACAMERA_OK
                || ACaptureSessionOutputContainer_add(container, output) != ACAMERA_OK) {
            ALOGE("Failed to add session output 0x%x", windows[i].flag);
            if (output) {
                ACaptureSessionOutput_free(output);
            }
            for (size_t j = 0; j < session_outputs.size(); ++j) {
                ACaptureSessionOutput_free(session_outputs[j]);
            }
            ACaptureSessionOutputContainer_free(container);
            return false;
        }
        session_outputs.push_back(output);
    }

#if ANDROID_MAJOR >= 9
    // Settings like the frame rate range are session parameters on some HALs
    ACaptureRequest *params = buildRequest(0, REQUEST_REPEATING);
    status = ACameraDevice_createCaptureSessionWithSessionParameters(m_device, container, params,
                                                                     &m_sessionCallbacks, &session);
    if (params) {
        ACaptureRequest_free(params);
    }
#else
    status = ACameraDevice_createCaptureSession(m_device, container, &m_sessionCallbacks, &session);
#endif

    // Creating a session closes the previous one in any case
    destroySession();

    if (status != ACAMERA_OK) {
        ALOGE("Failed to create capture session with outputs 0x%x: %d", outputs, status);
        for (size_t j = 0; j < session_outputs.size(); ++j) {
            ACaptureSessionOutput_free(session_outputs[j]);
        }
        ACaptureSessionOutputContainer_free(container);
        return false;
    }

    ALOGI("Created capture session with outputs 0x%x", outputs);
    m_session = session;
    m_container = container;
    m_sessionOutputs = session_outputs;
    m_sessionOutputFlags = outputs;
    m_sessionPreviewSize = previewSize();
    m_sessionPictureSize = m_jpegSize;
    m_sessionVideoSize = m_videoSize;
    return true;
}

void _DroidMediaCamera::destroySession()
{
    if (m_session) {
        ACameraCaptureSession_close(m_session);
        m_session = NULL;
    }
    for (size_t i = 0; i < m_sessionOutputs.size(); ++i) {
        ACaptureSessionOutput_free(m_sessionOutputs[i]);
    }
    m_sessionOutputs.clear();
    if (m_container) {
        ACaptureSessionOutputContainer_free(m_container);
        m_container = NULL;
    }
    m_sessionOutputFlags = 0;
    m_sessionWantedOutputs = 0;

    android::Mutex::Autolock locker(m_stateLock);
    m_repeatingTargets = 0;
}

void _DroidMediaCamera::visibleRect(int32_t rect[4]) const
{
    // left, top, width, height in active array coordinates
    rect[0] = 0;
    rect[1] = 0;
    rect[2] = m_activeArray[2];
    rect[3] = m_activeArray[3];

    if (m_zoomRatio) {
        // Regions and faces are relative to the zoomed field of view
        return;
    }

    int zoom = std::min(ZOOM_STEPS - 1, std::max(0, paramInt("zoom", 0)));
    float ratio = 1 + zoom * (m_maxZoom - 1) / (ZOOM_STEPS - 1);
    rect[2] = (int32_t)(m_activeArray[2] / ratio);
    rect[3] = (int32_t)(m_activeArray[3] / ratio);
    rect[0] = (m_activeArray[2] - rect[2]) / 2;
    rect[1] = (m_activeArray[3] - rect[3]) / 2;
}

void _DroidMediaCamera::applySettings(ACaptureRequest *request, int targets, RequestKind kind)
{
    uint8_t u8;

    // Scene
    if (lookup(SCENE_MODES, param("scene-mode"), &u8) && contains(m_sceneModes, u8)) {
        uint8_t mode = ACAMERA_CONTROL_MODE_USE_SCENE_MODE;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_MODE, 1, &mode);
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_SCENE_MODE, 1, &u8);
    } else {
        uint8_t mode = ACAMERA_CONTROL_MODE_AUTO;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_MODE, 1, &mode);
    }

    // Exposure and flash. The torch is a flash mode of the running camera.
    std::string flash = param("flash-mode");
    uint8_t ae_mode = ACAMERA_CONTROL_AE_MODE_ON;
    uint8_t flash_mode = ACAMERA_FLASH_MODE_OFF;
    if (m_flashAvailable) {
        if (flash == "auto") {
            ae_mode = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH;
        } else if (flash == "on") {
            ae_mode = ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
        } else if (flash == "red-eye") {
            ae_mode = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE;
        } else if (flash == "torch") {
            flash_mode = ACAMERA_FLASH_MODE_TORCH;
        }
        if (!contains(m_aeModes, ae_mode)) {
            ae_mode = ACAMERA_CONTROL_AE_MODE_ON;
        }
    }
    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &ae_mode);
    ACaptureRequest_setEntry_u8(request, ACAMERA_FLASH_MODE, 1, &flash_mode);

    int32_t compensation = paramInt("exposure-compensation", 0);
    compensation = std::min(m_compensationRange[1], std::max(m_compensationRange[0], compensation));
    ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &compensation);

    if (m_aeLockAvailable) {
        u8 = param("auto-exposure-lock") == "true"
            ? ACAMERA_CONTROL_AE_LOCK_ON : ACAMERA_CONTROL_AE_LOCK_OFF;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_LOCK, 1, &u8);
    }
    if (m_awbLockAvailable) {
        u8 = param("auto-whitebalance-lock") == "true"
            ? ACAMERA_CONTROL_AWB_LOCK_ON : ACAMERA_CONTROL_AWB_LOCK_OFF;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AWB_LOCK, 1, &u8);
    }

    if (lookup(ANTIBANDING_MODES, param("antibanding"), &u8) && contains(m_antibandingModes, u8)) {
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &u8);
    }
    if (lookup(WHITE_BALANCE_MODES, param("whitebalance"), &u8) && contains(m_awbModes, u8)) {
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AWB_MODE, 1, &u8);
    }
    if (lookup(EFFECT_MODES, param("effect"), &u8) && contains(m_effectModes, u8)) {
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_EFFECT_MODE, 1, &u8);
    }

    int min_fps, max_fps;
    if (sscanf(param("preview-fps-range").c_str(), "%d,%d", &min_fps, &max_fps) == 2) {
        int32_t range[2] = { min_fps / 1000, max_fps / 1000 };
        ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, range);
    }

    // Focus
    std::string focus = param("focus-mode");
    if (lookup(FOCUS_MODES, focus, &u8) && contains(m_afModes, u8)) {
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &u8);
        if (focus == "infinity") {
            float distance = 0;
            ACaptureRequest_setEntry_float(request, ACAMERA_LENS_FOCUS_DISTANCE, 1, &distance);
        }
    }

    // Zoom
    int zoom = std::min(ZOOM_STEPS - 1, std::max(0, paramInt("zoom", 0)));
    float ratio = 1 + zoom * (m_maxZoom - 1) / (ZOOM_STEPS - 1);
#if ANDROID_MAJOR >= 11
    if (m_zoomRatio) {
        ACaptureRequest_setEntry_float(request, ACAMERA_CONTROL_ZOOM_RATIO, 1, &ratio);
    } else
#endif
    if (m_activeArray[2] > 0 && ratio > 1) {
        int32_t crop[4];
        visibleRect(crop);
        ACaptureRequest_setEntry_i32(request, ACAMERA_SCALER_CROP_REGION, 4, crop);
    }

    // Focus and metering areas
    int32_t rect[4];
    visibleRect(rect);
    const struct {
        const char *key;
        uint32_t tag;
        int32_t max;
    } areas[] = {
        { "focus-areas", ACAMERA_CONTROL_AF_REGIONS, m_maxRegions[2] },
        { "metering-areas", ACAMERA_CONTROL_AE_REGIONS, m_maxRegions[0] },
    };
    for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); ++i) {
        std::vector<int32_t> regions = parse_areas(param(areas[i].key));
        if (regions.empty() || areas[i].max <= 0 || rect[2] <= 0) {
            continue;
        }
        regions.resize(std::min(regions.size(), (size_t)areas[i].max * 5));
        for (size_t j = 0; j < regions.size(); j += 5) {
            regions[j] = normalized_to_sensor(regions[j], rect[0], rect[2]);
            regions[j + 1] = normalized_to_sensor(regions[j + 1], rect[1], rect[3]);
            regions[j + 2] = normalized_to_sensor(regions[j + 2], rect[0], rect[2]);
            regions[j + 3] = normalized_to_sensor(regions[j + 3], rect[1], rect[3]);
        }
        ACaptureRequest_setEntry_i32(request, areas[i].tag, regions.size(), regions.data());
    }

    // Faces
    if (m_faceDetection) {
        u8 = contains(m_faceDetectModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL)
            ? ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL : ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE;
        if (contains(m_faceDetectModes, u8)) {
            ACaptureRequest_setEntry_u8(request, ACAMERA_STATISTICS_FACE_DETECT_MODE, 1, &u8);
        }
    }

    if ((targets & OUTPUT_VIDEO) && param("video-stabilization") == "true"
            && contains(m_stabilizationModes, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON)) {
        u8 = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &u8);
    }

    switch (kind) {
    case REQUEST_STILL: {
        u8 = std::min(100, std::max(1, paramInt("jpeg-quality", 95)));
        ACaptureRequest_setEntry_u8(request, ACAMERA_JPEG_QUALITY, 1, &u8);
        u8 = std::min(100, std::max(1, paramInt("jpeg-thumbnail-quality", 90)));
        ACaptureRequest_setEntry_u8(request, ACAMERA_JPEG_THUMBNAIL_QUALITY, 1, &u8);
        int32_t thumbnail[2] = { paramInt("jpeg-thumbnail-width", 0),
                                 paramInt("jpeg-thumbnail-height", 0) };
        ACaptureRequest_setEntry_i32(request, ACAMERA_JPEG_THUMBNAIL_SIZE, 2, thumbnail);
        int32_t orientation = paramInt("rotation", 0);
        ACaptureRequest_setEntry_i32(request, ACAMERA_JPEG_ORIENTATION, 1, &orientation);

        std::string latitude = param("gps-latitude");
        std::string longitude = param("gps-longitude");
        std::string altitude = param("gps-altitude");
        if (!latitude.empty() && !longitude.empty() && !altitude.empty()) {
            double coordinates[3] = { strtod(latitude.c_str(), NULL),
                                      strtod(longitude.c_str(), NULL),
                                      strtod(altitude.c_str(), NULL) };
            ACaptureRequest_setEntry_double(request, ACAMERA_JPEG_GPS_COORDINATES, 3, coordinates);
            std::string timestamp = param("gps-timestamp");
            if (!timestamp.empty()) {
                int64_t seconds = strtoll(timestamp.c_str(), NULL, 10);
                ACaptureRequest_setEntry_i64(request, ACAMERA_JPEG_GPS_TIMESTAMP, 1, &seconds);
            }
        }
        break;
    }
    case REQUEST_AF_START:
        u8 = ACAMERA_CONTROL_AF_TRIGGER_START;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &u8);
        break;
    case REQUEST_AF_CANCEL:
        u8 = ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &u8);
        break;
    case REQUEST_PRECAPTURE:
        u8 = ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START;
        ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &u8);
        break;
    case REQUEST_REPEATING:
        break;
    }
}

ACaptureRequest *_DroidMediaCamera::buildRequest(int targets, RequestKind kind)
{
    ACameraDevice_request_template request_template = TEMPLATE_PREVIEW;
    if (kind == REQUEST_STILL) {
        request_template = TEMPLATE_STILL_CAPTURE;
    } else if (targets & OUTPUT_VIDEO) {
        request_template = TEMPLATE_RECORD;
    }

    ACaptureRequest *request = NULL;
    if (ACameraDevice_createCaptureRequest(m_device, request_template, &request) != ACAMERA_OK) {
        ALOGE("Failed to create capture request from template %d", request_template);
        return NULL;
    }

    const struct {
        int flag;
        ACameraOutputTarget *target;
    } outputs[] = {
        { OUTPUT_PREVIEW, m_previewTarget },
        { OUTPUT_YUV, m_yuvTarget },
        { OUTPUT_JPEG, m_jpegTarget },
        { OUTPUT_VIDEO, m_videoTarget },
    };
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
        if ((targets & outputs[i].flag) && outputs[i].target) {
            ACaptureRequest_addTarget(request, outputs[i].target);
        }
    }

    // All settings go into every request: a still capture or trigger request
    // must not depend on what the repeating request happened to carry.
    applySettings(request, targets, kind);
    return request;
}

bool _DroidMediaCamera::updateRepeating()
{
    if (!m_previewRunning || !m_session) {
        return true;
    }

    int targets = OUTPUT_PREVIEW;
    if ((m_sessionOutputFlags & OUTPUT_YUV) && previewCallbacksEnabled()) {
        targets |= OUTPUT_YUV;
    }
    if ((m_sessionOutputFlags & OUTPUT_VIDEO) && m_videoRecording) {
        targets |= OUTPUT_VIDEO;
    }

    ACaptureRequest *request = buildRequest(targets, REQUEST_REPEATING);
    if (!request) {
        return false;
    }

    int sequence = -1;
    camera_status_t status = ACameraCaptureSession_setRepeatingRequest(m_session,
                                                                      &m_repeatingCallbacks, 1,
                                                                      &request, &sequence);
    ACaptureRequest_free(request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to set the repeating request: %d", status);
        return false;
    }

    uint8_t af_mode = ACAMERA_CONTROL_AF_MODE_OFF;
    lookup(FOCUS_MODES, param("focus-mode"), &af_mode);
    int32_t rect[4];
    visibleRect(rect);

    android::Mutex::Autolock locker(m_stateLock);
    m_repeatingSequence = sequence;
    m_repeatingTargets = targets;
    m_afMode = af_mode;
    m_facesEnabled = m_faceDetection;
    memcpy(m_faceRect, rect, sizeof(m_faceRect));
    return true;
}

void _DroidMediaCamera::deleteDeadReaders()
{
    std::vector<AImageReader *> readers;
    readers.swap(m_deadReaders);
    m_lock.unlock();

    for (size_t i = 0; i < readers.size(); ++i) {
        AImageReader_delete(readers[i]);
    }
}

bool _DroidMediaCamera::startPreview()
{
    if (m_previewRunning) {
        // Camera1 stops the preview for a picture and clients restart it, often
        // from the compressed image callback. The Camera2 preview keeps running.
        return true;
    }

    if (!m_videoRecording) {
        m_videoActive = false;
    }

    if (!ensureSession()) {
        return false;
    }

    m_previewRunning = true;
    if (!updateRepeating()) {
        m_previewRunning = false;
        return false;
    }

    return true;
}

void _DroidMediaCamera::stopPreview()
{
    if (!m_previewRunning) {
        return;
    }

    m_previewRunning = false;
    if (m_session) {
        ACameraCaptureSession_stopRepeating(m_session);
    }

    android::Mutex::Autolock locker(m_stateLock);
    m_afPending = false;
    m_precapturePending = false;
    if (m_pendingStill) {
        ACaptureRequest_free(m_pendingStill);
        m_pendingStill = NULL;
        m_stillPending = false;
    }
}

bool _DroidMediaCamera::takePicture(int msgType)
{
    if (!m_previewRunning) {
        ALOGE("Preview is not running");
        return false;
    }

    {
        android::Mutex::Autolock locker(m_stateLock);
        if (m_stillPending) {
            ALOGE("A picture is already being taken");
            return false;
        }
    }

    if (!(m_sessionOutputFlags & OUTPUT_JPEG) || m_jpegSize != pictureSize()) {
        if (m_videoRecording) {
            ALOGE("Taking pictures while recording is not supported");
            return false;
        }
        m_videoActive = false;
        if (!ensureSession() || !updateRepeating()) {
            return false;
        }
        if (!(m_sessionOutputFlags & OUTPUT_JPEG)) {
            ALOGE("No JPEG output in the capture session");
            return false;
        }
    }

    ACaptureRequest *still = buildRequest(OUTPUT_JPEG, REQUEST_STILL);
    if (!still) {
        return false;
    }

    std::string flash = param("flash-mode");
    bool precapture = m_flashAvailable && (flash == "auto" || flash == "on" || flash == "red-eye");

    {
        android::Mutex::Autolock locker(m_stateLock);
        m_stillPending = true;
        m_stillMsgType = msgType;
    }

    if (precapture) {
        // Let AE meter the flash before the still capture, like Camera1 HALs do
        ACaptureRequest *trigger = buildRequest(OUTPUT_PREVIEW, REQUEST_PRECAPTURE);
        if (trigger) {
            {
                android::Mutex::Autolock locker(m_stateLock);
                m_pendingStill = still;
                m_precapturePending = true;
                m_precaptureSeen = false;
                m_precaptureStarted = false;
                m_precaptureFrames = 0;
            }

            camera_status_t status = ACameraCaptureSession_capture(m_session, &m_precaptureCallbacks,
                                                                   1, &trigger, NULL);
            ACaptureRequest_free(trigger);
            if (status == ACAMERA_OK) {
                return true;
            }

            ALOGW("Failed to start the precapture sequence: %d", status);
            android::Mutex::Autolock locker(m_stateLock);
            m_pendingStill = NULL;
            m_precapturePending = false;
        }
    }

    camera_status_t status = ACameraCaptureSession_capture(m_session, &m_stillCallbacks, 1,
                                                           &still, NULL);
    ACaptureRequest_free(still);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to capture a still image: %d", status);
        android::Mutex::Autolock locker(m_stateLock);
        m_stillPending = false;
        return false;
    }

    return true;
}

void _DroidMediaCamera::submitStill(ACameraCaptureSession *session, ACaptureRequest *request)
{
    camera_status_t status = ACameraCaptureSession_capture(session, &m_stillCallbacks, 1,
                                                           &request, NULL);
    ACaptureRequest_free(request);

    if (status != ACAMERA_OK) {
        ALOGE("Failed to capture a still image: %d", status);
        bool closing;
        {
            android::Mutex::Autolock locker(m_stateLock);
            m_stillPending = false;
            closing = m_closing;
        }
        if (!closing && m_cb.error_cb) {
            m_cb.error_cb(m_cb_data, CAMERA_ERROR_UNKNOWN);
        }
    }
}

bool _DroidMediaCamera::startAutoFocus()
{
    if (!m_previewRunning || !m_session) {
        ALOGE("Preview is not running");
        return false;
    }

    uint8_t af_mode = ACAMERA_CONTROL_AF_MODE_OFF;
    lookup(FOCUS_MODES, param("focus-mode"), &af_mode);

    {
        android::Mutex::Autolock locker(m_stateLock);
        m_afPending = true;
        m_afFrames = 0;
        // Fixed focus succeeds with the next result, as it does with Camera1
        m_afTriggerSeen = af_mode == ACAMERA_CONTROL_AF_MODE_OFF
            || af_mode == ACAMERA_CONTROL_AF_MODE_EDOF;
        if (m_afTriggerSeen) {
            return true;
        }
    }

    ACaptureRequest *request = buildRequest(OUTPUT_PREVIEW, REQUEST_AF_START);
    if (!request) {
        android::Mutex::Autolock locker(m_stateLock);
        m_afPending = false;
        return false;
    }

    camera_status_t status = ACameraCaptureSession_capture(m_session, &m_afCallbacks, 1,
                                                           &request, NULL);
    ACaptureRequest_free(request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to trigger autofocus: %d", status);
        android::Mutex::Autolock locker(m_stateLock);
        m_afPending = false;
        return false;
    }

    return true;
}

bool _DroidMediaCamera::cancelAutoFocus()
{
    {
        android::Mutex::Autolock locker(m_stateLock);
        m_afPending = false;
    }

    if (!m_previewRunning || !m_session) {
        return true;
    }

    uint8_t af_mode = ACAMERA_CONTROL_AF_MODE_OFF;
    lookup(FOCUS_MODES, param("focus-mode"), &af_mode);
    if (af_mode == ACAMERA_CONTROL_AF_MODE_OFF || af_mode == ACAMERA_CONTROL_AF_MODE_EDOF) {
        return true;
    }

    ACaptureRequest *request = buildRequest(OUTPUT_PREVIEW, REQUEST_AF_CANCEL);
    if (!request) {
        return false;
    }

    camera_status_t status = ACameraCaptureSession_capture(m_session, NULL, 1, &request, NULL);
    ACaptureRequest_free(request);
    if (status != ACAMERA_OK) {
        ALOGE("Failed to cancel autofocus: %d", status);
        return false;
    }

    return true;
}

void _DroidMediaCamera::setPreviewCallbackFlags(int flags)
{
    bool was_enabled = previewCallbacksEnabled();
    m_previewCallbackFlags = flags;

    if (was_enabled == previewCallbacksEnabled() || !m_previewRunning) {
        return;
    }

    if (previewCallbacksEnabled() && !(m_sessionOutputFlags & OUTPUT_YUV)) {
        ensureSession();
    }
    updateRepeating();
}

bool _DroidMediaCamera::enableFaceDetection(bool enable)
{
    if (enable && !contains(m_faceDetectModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE)
            && !contains(m_faceDetectModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL)) {
        ALOGW("Face detection is not supported");
        return false;
    }

    if (m_faceDetection == enable) {
        return true;
    }

    m_faceDetection = enable;
    return updateRepeating();
}

ANativeWindow *_DroidMediaCamera::videoSurface()
{
    if (!m_videoWindow) {
        ANativeWindow *window = NULL;
        media_status_t status = AMediaCodec_createPersistentInputSurface(&window);
        if (status != AMEDIA_OK || !window) {
            ALOGE("Failed to create an encoder input surface: %d", status);
            return NULL;
        }
        if (ACameraOutputTarget_create(window, &m_videoTarget) != ACAMERA_OK) {
            ALOGE("Failed to create the video output target");
            ANativeWindow_release(window);
            return NULL;
        }
        m_videoWindow = window;
    }

    return m_videoWindow;
}

bool _DroidMediaCamera::configureVideo(int32_t width, int32_t height)
{
    if (!m_videoWindow) {
        return false;
    }

    // The encoder is configured on the surface by now, so the camera sees its
    // size and format when the session is created.
    m_videoSize = Size{ width, height };
    m_videoActive = true;

    if (!m_previewRunning) {
        return true;
    }

    return ensureSession() && (m_sessionOutputFlags & OUTPUT_VIDEO) && updateRepeating();
}

bool _DroidMediaCamera::startVideo()
{
    if (!m_videoWindow) {
        return false;
    }

    m_videoActive = true;
    m_videoRecording = true;

    if (!m_previewRunning) {
        // The encoder target is added when the preview starts
        return true;
    }

    if (!ensureSession() || !(m_sessionOutputFlags & OUTPUT_VIDEO) || !updateRepeating()) {
        ALOGE("Failed to add the encoder to the capture session");
        m_videoRecording = false;
        updateRepeating();
        return false;
    }

    return true;
}

void _DroidMediaCamera::stopVideo()
{
    if (!m_videoRecording) {
        return;
    }

    int sequence;
    bool had_target;
    {
        android::Mutex::Autolock locker(m_stateLock);
        sequence = m_repeatingSequence;
        had_target = m_repeatingTargets & OUTPUT_VIDEO;
    }

    m_videoRecording = false;
    updateRepeating();

    if (!had_target || sequence < 0) {
        return;
    }

    // Frames still in flight belong to this recording. Ending the encoder before
    // the capture sequence that fed it completes would lose them.
    {
        android::Mutex::Autolock locker(m_stateLock);
        nsecs_t deadline = systemTime(SYSTEM_TIME_MONOTONIC) + VIDEO_STOP_TIMEOUT_NS;
        while (m_finishedSequence < sequence && !m_closing) {
            nsecs_t remaining = deadline - systemTime(SYSTEM_TIME_MONOTONIC);
            if (remaining <= 0) {
                ALOGW("Timed out waiting for the recording capture sequence %d", sequence);
                break;
            }
            m_sequenceCond.waitRelative(m_stateLock, remaining);
        }
    }
    usleep(VIDEO_STOP_GRACE_US);
}

void _DroidMediaCamera::processResult(ACameraCaptureSession *session, const ACameraMetadata *result,
                                      bool afTrigger, bool precaptureTrigger)
{
    uint8_t af_state = ACAMERA_CONTROL_AF_STATE_INACTIVE;
    uint8_t ae_state = ACAMERA_CONTROL_AE_STATE_INACTIVE;
    bool have_af_state = metadata_first_u8(result, ACAMERA_CONTROL_AF_STATE, &af_state);
    bool have_ae_state = metadata_first_u8(result, ACAMERA_CONTROL_AE_STATE, &ae_state);

    bool focus_done = false;
    bool focus_success = false;
    int focus_move = -1;
    bool faces = false;
    ACaptureRequest *still = NULL;

    {
        android::Mutex::Autolock locker(m_stateLock);
        if (m_closing) {
            return;
        }

        if (afTrigger) {
            m_afTriggerSeen = true;
        }
        if (m_afPending && m_afTriggerSeen) {
            if (m_afMode == ACAMERA_CONTROL_AF_MODE_OFF || m_afMode == ACAMERA_CONTROL_AF_MODE_EDOF) {
                focus_done = focus_success = true;
            } else if (have_af_state && af_state == ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED) {
                focus_done = focus_success = true;
            } else if (have_af_state && af_state == ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED) {
                focus_done = true;
            } else if (++m_afFrames > AF_TIMEOUT_FRAMES) {
                ALOGW("Timed out waiting for autofocus");
                focus_done = true;
            }
            if (focus_done) {
                m_afPending = false;
            }
        }

        if (m_focusMoveEnabled && have_af_state
                && (m_afMode == ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE
                    || m_afMode == ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO)) {
            bool moving = af_state == ACAMERA_CONTROL_AF_STATE_PASSIVE_SCAN;
            if (moving != m_focusMoving) {
                m_focusMoving = moving;
                focus_move = moving ? 1 : 0;
            }
        }

        // Wait for AE to enter and then leave the precapture state
        if (precaptureTrigger) {
            m_precaptureSeen = true;
        }
        if (m_precapturePending && m_precaptureSeen) {
            ++m_precaptureFrames;
            if (!m_precaptureStarted) {
                m_precaptureStarted = !have_ae_state
                    || ae_state == ACAMERA_CONTROL_AE_STATE_PRECAPTURE
                    || ae_state == ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED
                    || m_precaptureFrames > PRECAPTURE_START_FRAMES;
            }
            if ((m_precaptureStarted && ae_state != ACAMERA_CONTROL_AE_STATE_PRECAPTURE)
                    || m_precaptureFrames > PRECAPTURE_TIMEOUT_FRAMES) {
                still = m_pendingStill;
                m_pendingStill = NULL;
                m_precapturePending = false;
            }
        }

        faces = m_facesEnabled;
    }

    if (focus_done && m_cb.focus_cb) {
        m_cb.focus_cb(m_cb_data, focus_success ? 1 : 0);
    }
    if (focus_move >= 0 && m_cb.focus_move_cb) {
        m_cb.focus_move_cb(m_cb_data, focus_move);
    }
    if (still) {
        submitStill(session, still);
    }
    if (faces && !afTrigger && !precaptureTrigger) {
        sendFaces(result);
    }
}

void _DroidMediaCamera::sendFaces(const ACameraMetadata *result)
{
    std::vector<int32_t> rects = metadata_i32(result, ACAMERA_STATISTICS_FACE_RECTANGLES);
    std::vector<uint8_t> scores = metadata_u8(result, ACAMERA_STATISTICS_FACE_SCORES);
    std::vector<int32_t> ids = metadata_i32(result, ACAMERA_STATISTICS_FACE_IDS);
    std::vector<int32_t> landmarks = metadata_i32(result, ACAMERA_STATISTICS_FACE_LANDMARKS);
    std::vector<DroidMediaCameraFace> faces;

    int32_t rect[4];
    size_t last_count;
    {
        android::Mutex::Autolock locker(m_stateLock);
        memcpy(rect, m_faceRect, sizeof(rect));
        last_count = m_lastFaceCount;
    }

    for (size_t i = 0; i * 4 + 3 < rects.size(); ++i) {
        // Rectangles are left, top, right, bottom in active array coordinates
        DroidMediaCameraFace face;
        face.left = sensor_to_normalized(rects[i * 4], rect[0], rect[2]);
        face.top = sensor_to_normalized(rects[i * 4 + 1], rect[1], rect[3]);
        face.right = sensor_to_normalized(rects[i * 4 + 2], rect[0], rect[2]);
        face.bottom = sensor_to_normalized(rects[i * 4 + 3], rect[1], rect[3]);
        face.score = i < scores.size() ? scores[i] : 100;
        face.id = i < ids.size() ? ids[i] : -1;
        // Camera1 reports missing landmarks as -2000
        int32_t *points[3] = { face.left_eye, face.right_eye, face.mouth };
        for (int p = 0; p < 3; ++p) {
            if (i * 6 + 5 < landmarks.size()) {
                points[p][0] = sensor_to_normalized(landmarks[i * 6 + p * 2], rect[0], rect[2]);
                points[p][1] = sensor_to_normalized(landmarks[i * 6 + p * 2 + 1], rect[1], rect[3]);
            } else {
                points[p][0] = points[p][1] = -2000;
            }
        }
        faces.push_back(face);
    }

    {
        android::Mutex::Autolock locker(m_stateLock);
        m_lastFaceCount = faces.size();
    }

    // Report faces and, once, that they are gone
    if ((faces.size() || last_count) && m_cb.preview_metadata_cb) {
        m_cb.preview_metadata_cb(m_cb_data, faces.data(), faces.size());
    }
}

void _DroidMediaCamera::yuvImageAvailable(AImageReader *reader)
{
    AImage *image = NULL;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) {
        return;
    }

    int flags = m_previewCallbackFlags;
    if ((flags & CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK) && m_cb.preview_frame_cb
            && image_to_nv21(image, m_nv21)) {
        if (flags & CAMERA_FRAME_CALLBACK_FLAG_ONE_SHOT_MASK) {
            m_previewCallbackFlags = CAMERA_FRAME_CALLBACK_FLAG_NOOP;
        }

        DroidMediaData mem;
        mem.data = m_nv21.data();
        mem.size = m_nv21.size();
        m_cb.preview_frame_cb(m_cb_data, &mem);
    }

    AImage_delete(image);
}

void _DroidMediaCamera::jpegImageAvailable(AImageReader *reader)
{
    AImage *image = NULL;
    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || !image) {
        return;
    }

    int msgType;
    {
        android::Mutex::Autolock locker(m_stateLock);
        m_stillPending = false;
        msgType = m_stillMsgType;
    }

    uint8_t *data = NULL;
    int length = 0;
    if (AImage_getPlaneData(image, 0, &data, &length) != AMEDIA_OK || !data || length <= 0) {
        ALOGE("Failed to get the JPEG data");
        AImage_delete(image);
        if (m_cb.error_cb) {
            m_cb.error_cb(m_cb_data, CAMERA_ERROR_UNKNOWN);
        }
        return;
    }

    // The client may restart the preview and take the next picture from here
    if ((msgType & CAMERA_MSG_COMPRESSED_IMAGE) && m_cb.compressed_image_cb) {
        DroidMediaData mem;
        mem.data = data;
        mem.size = length;
        m_cb.compressed_image_cb(m_cb_data, &mem);
    }

    AImage_delete(image);
}

void _DroidMediaCamera::deviceError(int error)
{
    {
        android::Mutex::Autolock locker(m_stateLock);
        if (m_closing) {
            return;
        }
    }

    int camera_error = CAMERA_ERROR_UNKNOWN;
    if (error < 0) {
        ALOGE("Camera %s disconnected", m_id.c_str());
        camera_error = CAMERA_ERROR_RELEASED;
    } else {
        ALOGE("Camera %s error %d", m_id.c_str(), error);
        if (error == ERROR_CAMERA_SERVICE) {
            camera_error = CAMERA_ERROR_SERVER_DIED;
        }
    }

    if (m_cb.error_cb) {
        m_cb.error_cb(m_cb_data, camera_error);
    }
}

extern "C" {

DroidMediaBufferQueue *droid_media_camera_get_buffer_queue (DroidMediaCamera *camera)
{
    return camera->m_queue.get();
}

DroidMediaBufferQueue *droid_media_camera_get_recording_buffer_queue (DroidMediaCamera *camera)
{
    // Video goes straight to the encoder, see droidmediarecorder2.cpp
    return NULL;
}

int droid_media_camera_get_number_of_cameras()
{
    return camera_ids().size();
}

bool droid_media_camera_get_info(DroidMediaCameraInfo *info, int camera_number)
{
    ACameraMetadata *chars = camera_characteristics(camera_number, NULL);
    if (!chars) {
        return false;
    }

    uint8_t facing = ACAMERA_LENS_FACING_BACK;
    metadata_first_u8(chars, ACAMERA_LENS_FACING, &facing);
    std::vector<int32_t> orientation = metadata_i32(chars, ACAMERA_SENSOR_ORIENTATION);
    ACameraMetadata_free(chars);

    info->facing = facing == ACAMERA_LENS_FACING_FRONT
        ? DROID_MEDIA_CAMERA_FACING_FRONT : DROID_MEDIA_CAMERA_FACING_BACK;
    info->orientation = orientation.empty() ? 0 : orientation[0];
    return true;
}

DroidMediaCamera *droid_media_camera_connect(int camera_number)
{
    DroidMediaCamera *cam = new DroidMediaCamera;
    if (!cam) {
        ALOGE("Failed to allocate DroidMediaCamera");
        return NULL;
    }

    if (!cam->open(camera_number)) {
        ALOGE("Failed to connect to camera %d", camera_number);
        cam->close();
        delete cam;
        return NULL;
    }

    return cam;
}

bool droid_media_camera_reconnect(DroidMediaCamera *camera)
{
    // The device stays open for as long as the camera is connected
    return camera->m_device != NULL;
}

void droid_media_camera_disconnect(DroidMediaCamera *camera)
{
    camera->close();
    delete camera;
}

bool droid_media_camera_lock(DroidMediaCamera *camera)
{
    return true;
}

bool droid_media_camera_unlock(DroidMediaCamera *camera)
{
    return true;
}

bool droid_media_camera_start_preview(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->startPreview();
}

void droid_media_camera_stop_preview(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    camera->stopPreview();
}

bool droid_media_camera_is_preview_enabled(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->m_previewRunning;
}

bool droid_media_camera_start_recording(DroidMediaCamera *camera)
{
    ALOGE("Recording frame callbacks are not supported, use DroidMediaRecorder");
    return false;
}

void droid_media_camera_stop_recording(DroidMediaCamera *camera)
{
}

bool droid_media_camera_is_recording_enabled(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->m_videoRecording;
}

bool droid_media_camera_start_auto_focus(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->startAutoFocus();
}

bool droid_media_camera_cancel_auto_focus(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->cancelAutoFocus();
}

void droid_media_camera_set_callbacks(DroidMediaCamera *camera, DroidMediaCameraCallbacks *cb, void *data)
{
    memcpy(&camera->m_cb, cb, sizeof(camera->m_cb));
    camera->m_cb_data = data;
}

bool droid_media_camera_send_command(DroidMediaCamera *camera, int32_t cmd, int32_t arg1, int32_t arg2)
{
    switch (cmd) {
    case CAMERA_CMD_ENABLE_SHUTTER_SOUND:
    case CAMERA_CMD_PLAY_RECORDING_SOUND:
    case CAMERA_CMD_SET_DISPLAY_ORIENTATION:
    case CAMERA_CMD_PING:
        // Sounds and display orientation are up to the client
        return true;

    case CAMERA_CMD_ENABLE_FOCUS_MOVE_MSG: {
        android::Mutex::Autolock locker(camera->m_stateLock);
        camera->m_focusMoveEnabled = arg1 != 0;
        camera->m_focusMoving = false;
        return true;
    }

    case CAMERA_CMD_START_FACE_DETECTION:
    case CAMERA_CMD_STOP_FACE_DETECTION:
        return droid_media_camera_enable_face_detection(camera,
                arg1 == CAMERA_FACE_DETECTION_SW ? DROID_MEDIA_CAMERA_FACE_DETECTION_SW
                : DROID_MEDIA_CAMERA_FACE_DETECTION_HW,
                cmd == CAMERA_CMD_START_FACE_DETECTION);

    default:
        ALOGW("Unsupported command %d (%d, %d)", cmd, arg1, arg2);
        return false;
    }
}

bool droid_media_camera_store_meta_data_in_buffers(DroidMediaCamera *camera, bool enabled)
{
    // Only DroidMediaRecorder records video
    return !enabled;
}

void droid_media_camera_set_preview_callback_flags(DroidMediaCamera *camera, int preview_callback_flag)
{
    ApiLock locker(camera);
    camera->setPreviewCallbackFlags(preview_callback_flag);
}

bool droid_media_camera_set_parameters(DroidMediaCamera *camera, const char *params)
{
    ApiLock locker(camera);
    return camera->setParameters(params);
}

char *droid_media_camera_get_parameters(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->flattenParameters();
}

bool droid_media_camera_take_picture(DroidMediaCamera *camera, int msgType)
{
    ApiLock locker(camera);
    return camera->takePicture(msgType);
}

void droid_media_camera_release_recording_frame(DroidMediaCamera *camera, DroidMediaCameraRecordingData *data)
{
    delete data;
}

nsecs_t droid_media_camera_recording_frame_get_timestamp(DroidMediaCameraRecordingData *data)
{
    return data->ts;
}

size_t droid_media_camera_recording_frame_get_size(DroidMediaCameraRecordingData *data)
{
    return 0;
}

void *droid_media_camera_recording_frame_get_data(DroidMediaCameraRecordingData *data)
{
    return NULL;
}

bool droid_media_camera_enable_face_detection(DroidMediaCamera *camera,
					      DroidMediaCameraFaceDetectionType type, bool enable)
{
    if (enable && type != DROID_MEDIA_CAMERA_FACE_DETECTION_HW) {
        ALOGW("Only hardware face detection is supported");
        return false;
    }

    ApiLock locker(camera);
    return camera->enableFaceDetection(enable);
}

int32_t droid_media_camera_get_video_color_format (DroidMediaCamera *camera)
{
    // The format of preview_frame_cb data
    return OMX_COLOR_FormatYUV420SemiPlanar;
}

};

bool droid_media_camera_get_id(int camera_number, std::string *id)
{
    std::vector<std::string> ids = camera_ids();
    if (camera_number < 0 || camera_number >= (int)ids.size()) {
        return false;
    }
    *id = ids[camera_number];
    return true;
}

ANativeWindow *droid_media_camera2_get_video_surface(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->videoSurface();
}

bool droid_media_camera2_configure_video(DroidMediaCamera *camera, int32_t width, int32_t height)
{
    ApiLock locker(camera);
    return camera->configureVideo(width, height);
}

bool droid_media_camera2_start_video(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    return camera->startVideo();
}

void droid_media_camera2_stop_video(DroidMediaCamera *camera)
{
    ApiLock locker(camera);
    camera->stopVideo();
}
