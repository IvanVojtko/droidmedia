/*
 * Copyright (C) 2016 Jolla Ltd.
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
 * DroidMediaRecorder for the Camera2 backend: the camera renders into a
 * persistent input surface of an NDK MediaCodec encoder.
 */

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include "droidmediarecorder.h"
#include "private.h"

#undef LOG_TAG
#define LOG_TAG "DroidMediaRecorder"

namespace {

// MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface
const int32_t COLOR_FORMAT_SURFACE = 0x7F000789;
// MediaCodec.BUFFER_FLAG_KEY_FRAME, not in older NDK headers
const uint32_t BUFFER_FLAG_KEY_FRAME = 1;
const int64_t DEQUEUE_TIMEOUT_US = 10000;
const int64_t EOS_TIMEOUT_US = 2000000;

int64_t clock_us(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

}

struct _DroidMediaRecorder {
  _DroidMediaRecorder() :
    m_cam(0),
    m_codec(0),
    m_cb_data(0),
    m_running(false),
    m_started(false),
    m_stopping(false),
    m_startMonotonicUs(0),
    m_startBoottimeUs(0),
    m_baseUs(-1),
    m_lastDtsUs(-1),
    m_stopDeadlineUs(0) {
    memset(&m_cb, 0x0, sizeof(m_cb));
  }

  // Encoder frames carry the timestamps the camera gave them. Those can be on
  // CLOCK_BOOTTIME or CLOCK_MONOTONIC depending on the HAL, so the clock is
  // picked from the first buffer and timestamps start from the recording start.
  int64_t timestamp(int64_t ptsUs) {
    if (m_baseUs < 0) {
      int64_t monotonic = clock_us(CLOCK_MONOTONIC);
      int64_t boottime = clock_us(CLOCK_BOOTTIME);
      bool use_boottime = llabs(boottime - ptsUs) < llabs(monotonic - ptsUs);
      m_baseUs = use_boottime ? m_startBoottimeUs : m_startMonotonicUs;
      ALOGI("Video timestamps are on %s", use_boottime ? "CLOCK_BOOTTIME" : "CLOCK_MONOTONIC");
    }
    return ptsUs > m_baseUs ? ptsUs - m_baseUs : 0;
  }

  bool tick() {
    AMediaCodecBufferInfo info;
    ssize_t index = AMediaCodec_dequeueOutputBuffer(m_codec, &info, DEQUEUE_TIMEOUT_US);

    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      if (m_stopping && clock_us(CLOCK_MONOTONIC) > m_stopDeadlineUs) {
        ALOGW("Timed out waiting for the end of the video stream");
        return false;
      }
      return true;
    } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED
               || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      return true;
    } else if (index < 0) {
      ALOGE("Error %zd dequeuing an encoded buffer", index);
      return false;
    }

    size_t size = 0;
    uint8_t *buffer = AMediaCodec_getOutputBuffer(m_codec, index, &size);

    if (buffer && info.size > 0 && (size_t)(info.offset + info.size) <= size) {
      DroidMediaCodecData data;
      data.data.data = buffer + info.offset;
      data.data.size = info.size;
      data.codec_config = info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG;
      data.sync = info.flags & BUFFER_FLAG_KEY_FRAME;
      data.ts = 0;
      data.decoding_ts = 0;

      if (!data.codec_config) {
        int64_t ptsUs = timestamp(info.presentationTimeUs);
        // The NDK has no decoding timestamps. Without B-frames they equal the
        // presentation timestamps, keep them increasing in any case.
        int64_t dtsUs = ptsUs > m_lastDtsUs ? ptsUs : m_lastDtsUs + 1;
        m_lastDtsUs = dtsUs;
        // Convert from usec to nsec
        data.ts = ptsUs * 1000;
        data.decoding_ts = dtsUs * 1000;
      }

      if (m_cb.data_available) {
        m_cb.data_available(m_cb_data, &data);
      }
    }

    AMediaCodec_releaseOutputBuffer(m_codec, index, false);

    return !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
  }

  void *run() {
    while (m_running && tick()) {
    }
    return NULL;
  }

  static void *ThreadWrapper(void *that) {
    return static_cast<DroidMediaRecorder *>(that)->run();
  }

  DroidMediaCamera *m_cam;
  AMediaCodec *m_codec;
  DroidMediaCodecDataCallbacks m_cb;
  void *m_cb_data;
  volatile bool m_running;
  bool m_started;
  volatile bool m_stopping;
  int64_t m_startMonotonicUs;
  int64_t m_startBoottimeUs;
  int64_t m_baseUs;
  int64_t m_lastDtsUs;
  int64_t m_stopDeadlineUs;
  pthread_t m_thread;
};

static AMediaCodec *create_encoder(DroidMediaCodecEncoderMetaData *meta, bool tuned)
{
  AMediaCodec *codec = AMediaCodec_createEncoderByType(meta->parent.type);
  if (!codec) {
    ALOGE("No encoder for %s", meta->parent.type);
    return NULL;
  }

  AMediaFormat *format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, meta->parent.type);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, meta->parent.width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, meta->parent.height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, meta->bitrate);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE,
                        meta->parent.fps > 0 ? meta->parent.fps : 30);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FORMAT_SURFACE);

  if (tuned) {
    if (meta->bitrate_mode != DROID_MEDIA_CODEC_BITRATE_CONTROL_DEFAULT) {
      AMediaFormat_setInt32(format, "bitrate-mode", meta->bitrate_mode);
    }
    if (!strcmp(meta->parent.type, "video/avc")) {
      // The OMX AVC profile and level values are the MediaCodec ones
      if (meta->codec_specific.h264.profile) {
        AMediaFormat_setInt32(format, "profile", meta->codec_specific.h264.profile);
      }
      if (meta->codec_specific.h264.level) {
        AMediaFormat_setInt32(format, "level", meta->codec_specific.h264.level);
      }
      if (meta->codec_specific.h264.prepend_header_to_sync_frames) {
        AMediaFormat_setInt32(format, "prepend-sps-pps-to-idr-frames", 1);
      }
    }
    // Real time
    AMediaFormat_setInt32(format, "priority", 0);
  }

  media_status_t status = AMediaCodec_configure(codec, format, NULL, NULL,
                                                AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(format);

  if (status != AMEDIA_OK) {
    ALOGW("Failed to configure %s encoder%s: %d", meta->parent.type,
          tuned ? " with tuned settings" : "", status);
    AMediaCodec_delete(codec);
    return NULL;
  }

  return codec;
}

extern "C" {
DroidMediaRecorder *droid_media_recorder_create(DroidMediaCamera *camera, DroidMediaCodecEncoderMetaData *meta) {

  ANativeWindow *surface = droid_media_camera2_get_video_surface(camera);
  if (!surface) {
    return NULL;
  }

  // A refused configure can leave the codec unusable, so retry with a new one
  // and the encoder defaults.
  AMediaCodec *codec = create_encoder(meta, true);
  if (!codec) {
    codec = create_encoder(meta, false);
  }
  if (!codec) {
    ALOGE("Cannot create codec");
    return NULL;
  }

  media_status_t status = AMediaCodec_setInputSurface(codec, surface);
  if (status != AMEDIA_OK) {
    ALOGE("Failed to set the encoder input surface: %d", status);
    AMediaCodec_delete(codec);
    return NULL;
  }

  // Now that the surface has the encoder's size and format, the camera can
  // add it to the capture session.
  if (!droid_media_camera2_configure_video(camera, meta->parent.width, meta->parent.height)) {
    ALOGE("Failed to add the encoder to the camera");
    AMediaCodec_delete(codec);
    return NULL;
  }

  meta->color_format = COLOR_FORMAT_SURFACE;
  meta->meta_data = false;

  DroidMediaRecorder *recorder = new DroidMediaRecorder;
  recorder->m_cam = camera;
  recorder->m_codec = codec;

  return recorder;
}

void droid_media_recorder_destroy(DroidMediaRecorder *recorder) {
  if (recorder->m_started) {
    droid_media_recorder_stop(recorder);
  }

  AMediaCodec_delete(recorder->m_codec);
  delete recorder;
}

bool droid_media_recorder_start(DroidMediaRecorder *recorder) {

  media_status_t status = AMediaCodec_start(recorder->m_codec);
  if (status != AMEDIA_OK) {
    ALOGE("error %d starting codec", status);
    return false;
  }

  recorder->m_startMonotonicUs = clock_us(CLOCK_MONOTONIC);
  recorder->m_startBoottimeUs = clock_us(CLOCK_BOOTTIME);
  recorder->m_baseUs = -1;
  recorder->m_lastDtsUs = -1;
  recorder->m_stopping = false;
  recorder->m_running = true;
  recorder->m_started = true;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  pthread_create(&recorder->m_thread, &attr, DroidMediaRecorder::ThreadWrapper, recorder);
  pthread_attr_destroy(&attr);

  if (!droid_media_camera2_start_video(recorder->m_cam)) {
    ALOGE("Failed to start sending frames to the encoder");
    droid_media_recorder_stop(recorder);
    return false;
  }

  return true;
}

void droid_media_recorder_stop(DroidMediaRecorder *recorder) {
  if (!recorder->m_started) {
    return;
  }

  // Waits until the frames in flight have reached the encoder
  droid_media_camera2_stop_video(recorder->m_cam);

  recorder->m_stopDeadlineUs = clock_us(CLOCK_MONOTONIC) + EOS_TIMEOUT_US;
  recorder->m_stopping = true;
  media_status_t status = AMediaCodec_signalEndOfInputStream(recorder->m_codec);
  if (status != AMEDIA_OK) {
    ALOGE("error %d signalling the end of the video stream", status);
    recorder->m_running = false;
  }

  void *dummy;
  pthread_join(recorder->m_thread, &dummy);
  recorder->m_running = false;
  recorder->m_started = false;

  status = AMediaCodec_stop(recorder->m_codec);
  if (status != AMEDIA_OK) {
    ALOGE("error %d stopping codec", status);
  }
}

void droid_media_recorder_set_data_callbacks(DroidMediaRecorder *recorder,
					     DroidMediaCodecDataCallbacks *cb, void *data) {
  memcpy(&recorder->m_cb, cb, sizeof(recorder->m_cb));
  recorder->m_cb_data = data;
}

};
