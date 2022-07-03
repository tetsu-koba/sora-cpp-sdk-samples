/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pipe_video_capturer.h"

// C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// C++
#include <new>
#include <string>

// Linux
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

// WebRTC
#include <api/scoped_refptr.h>
#include <api/video/i420_buffer.h>
#include <rtc_base/logging.h>
#include <rtc_base/ref_counted_object.h>

rtc::scoped_refptr<PipeVideoCapturer> PipeVideoCapturer::Create(
    PipeVideoCapturerConfig config) {
  rtc::scoped_refptr<PipeVideoCapturer> pipe_capturer(
      new rtc::RefCountedObject<PipeVideoCapturer>());
  if (pipe_capturer->Init(config.video_fifo) < 0) {
    RTC_LOG(LS_WARNING) << "Failed to create PipeVideoCapturer(video_fifo = " << config.video_fifo << ")";
    return nullptr;
  }
  if (pipe_capturer->StartCapture(config) < 0) {
    RTC_LOG(LS_WARNING) << "Failed to start PipeVideoCapturer(w = "
                        << config.width << ", h = " << config.height
                        << ", fps = " << config.framerate << ")";
    return nullptr;
  }
  return pipe_capturer;
}


PipeVideoCapturer::PipeVideoCapturer()
    : _fd(-1),
      _width(-1),
      _height(-1),
      _framerate(-1),
      _captureStarted(false){}

int32_t PipeVideoCapturer::Init(const std::string& pipeName) {
  if (0 == pipeName.compare("-")) {
    _fd = 0;
  } else if ((_fd = open(pipeName.c_str(), O_RDONLY)) < 0) {
    RTC_LOG(LS_INFO) << "error in opening " << pipeName
                     << " errono = " << errno;
    return -1;
  }
  struct stat st;
  if (!fstat(_fd, &st) && S_ISFIFO(st.st_mode)) {
    int pipe_size = 1024 * 1024;
    auto f = fopen ("/proc/sys/fs/pipe-max-size" , "r");
    if (f != NULL) {
      char buf[128];
      if (fgets(buf, sizeof(buf), f)) {
	pipe_size = atoi(buf);
      }
    }
    fcntl(_fd, F_SETPIPE_SZ, pipe_size);
  }
  return 0;
}

PipeVideoCapturer::~PipeVideoCapturer() {
  StopCapture();
  if (_fd != -1 && _fd != 0)
    close(_fd);
}

int32_t PipeVideoCapturer::StartCapture(PipeVideoCapturerConfig config) {
  if (_captureStarted) {
    if (config.width == _width && config.height == _height) {
      return 0;
    } else {
      StopCapture();
    }
  }

  webrtc::MutexLock lock(&capture_lock_);

  _width = config.width;
  _height = config.height;
  _framerate = config.framerate;
  if (!AllocateVideoBuffers()) {
    RTC_LOG(LS_INFO) << "failed to allocate video capture buffers";
    return -1;
  }

  // start capture thread;
  if (_captureThread.empty()) {
    quit_ = false;
    _captureThread = rtc::PlatformThread::SpawnJoinable(
        std::bind(PipeVideoCapturer::CaptureThread, this), "CaptureThread",
        rtc::ThreadAttributes().SetPriority(rtc::ThreadPriority::kHigh));
  }

  _captureStarted = true;
  return 0;
}

int32_t PipeVideoCapturer::StopCapture() {
  if (!_captureThread.empty()) {
    {
      webrtc::MutexLock lock(&capture_lock_);
      quit_ = true;
    }
    _captureThread.Finalize();
  }

  webrtc::MutexLock lock(&capture_lock_);
  if (_captureStarted) {
    _captureStarted = false;

    DeAllocateVideoBuffers();
    if (_fd != 0) {
      close(_fd);
      _fd = -1;
    }
  }

  return 0;
}

// critical section protected by the caller

bool PipeVideoCapturer::AllocateVideoBuffers() {
  _bufsize = _width * (_height + _height / 2);
  _buffer = nullptr;
  try {
    _buffer = new uint8_t[_bufsize];
  } catch (const std::bad_alloc& e) {
    return false;
  }
  return true;
}

bool PipeVideoCapturer::DeAllocateVideoBuffers() {
  delete _buffer;
  return true;
}

void PipeVideoCapturer::CaptureThread(void* obj) {
  PipeVideoCapturer* capturer = static_cast<PipeVideoCapturer*>(obj);
  while (capturer->CaptureProcess()) {
  }
}

bool PipeVideoCapturer::CaptureProcess() {
  int retVal = 0;
  fd_set rSet;
  struct timeval timeout;

  FD_ZERO(&rSet);
  FD_SET(_fd, &rSet);
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;

  retVal = select(_fd + 1, &rSet, NULL, NULL, &timeout);
  {
    webrtc::MutexLock lock(&capture_lock_);

    if (quit_) {
      return false;
    } else if (retVal < 0 && errno != EINTR /* continue if interrupted */) {
      // select failed
      return false;
    } else if (retVal == 0) {
      // select timed out
      return true;
    } else if (!FD_ISSET(_fd, &rSet)) {
      // not event on pipe handle
      return true;
    }

    if (_captureStarted) {
      int bytes_read = 0;
      int to_read = _bufsize;
      while (to_read > 0) {
	errno = 0;
	int ret = read(_fd, _buffer + bytes_read, to_read);
	if (ret < 0) {
	  if (errno == EAGAIN || errno == EINTR)
	    continue;
	  return false;
	}
	if (ret == 0) {
	  if (bytes_read > 0)
	    break;
	  return false;
	}
	to_read -= ret;
	bytes_read += ret;
      }
      if (bytes_read == _bufsize) {
	OnCaptured(_buffer);
      }
    }
  }
  usleep(0); // yeild
  return true;
}

void PipeVideoCapturer::OnCaptured(uint8_t* data) {
  auto w = _width;
  auto h = _height;
  auto i420_buffer = webrtc::I420Buffer::Copy(w, h,
					      data, w,
					      data + (w * h), w/2,
					      data + (w * h) + (w/2 * h/2), w/2);
  webrtc::VideoFrame video_frame = webrtc::VideoFrame::Builder()
    .set_video_frame_buffer(i420_buffer)
    .set_timestamp_rtp(0)
    .set_timestamp_ms(rtc::TimeMillis())
    .set_timestamp_us(rtc::TimeMicros())
    .set_rotation(webrtc::kVideoRotation_0)
    .build();
  OnCapturedFrame(video_frame);
}
