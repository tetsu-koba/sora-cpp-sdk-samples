/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SORA_PIPE_VIDEO_CAPTURER_H_
#define SORA_PIPE_VIDEO_CAPTURER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>

// WebRTC
#include <modules/video_capture/video_capture_defines.h>
#include <modules/video_capture/video_capture_impl.h>
#include <rtc_base/platform_thread.h>
#include <rtc_base/synchronization/mutex.h>

#include <sora/scalable_track_source.h>

struct PipeVideoCapturerConfig {
  std::string video_fifo;
  int width = 640;
  int height = 480;
  int framerate = 30;
};

class PipeVideoCapturer : public sora::ScalableVideoTrackSource {
 public:
  static rtc::scoped_refptr<PipeVideoCapturer> Create(
      PipeVideoCapturerConfig config);
  PipeVideoCapturer();
  ~PipeVideoCapturer();

  int32_t Init(const std::string& pipeName);
  virtual int32_t StartCapture(PipeVideoCapturerConfig config);

 protected:
  virtual int32_t StopCapture();
  virtual bool AllocateVideoBuffers();
  virtual bool DeAllocateVideoBuffers();
  virtual void OnCaptured(uint8_t* data);

  int32_t _fd;
  int32_t _width;
  int32_t _height;
  int32_t _framerate;
  int32_t _bufsize;
  uint8_t* _buffer;

 private:
  static rtc::scoped_refptr<PipeVideoCapturer> Create(
      webrtc::VideoCaptureModule::DeviceInfo* device_info,
      PipeVideoCapturerConfig config,
      size_t capture_device_index);

  static void CaptureThread(void*);
  bool CaptureProcess();

  rtc::PlatformThread _captureThread;
  webrtc::Mutex capture_lock_;
  bool quit_ RTC_GUARDED_BY(capture_lock_);

  bool _captureStarted;
};

#endif
