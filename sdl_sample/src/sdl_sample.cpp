// Sora
#include <sora/camera_device_capturer.h>
#include <sora/sora_default_client.h>

#include <regex>

// CLI11
#include <CLI/CLI.hpp>

#include "sdl_renderer.h"

#ifdef _WIN32
#include <rtc_base/win/scoped_com_initializer.h>
#endif

std::string GenerateRandomChars(size_t length) {
  std::string result;
  rtc::CreateRandomString(length, &result);
  return result;
}

std::string GenerateRandomChars() {
  return GenerateRandomChars(32);
}

struct SDLSampleConfig : sora::SoraDefaultClientConfig {
  std::string signaling_url;
  std::string channel_id;
  std::string role;
  std::string video_codec_type;
  std::string metadata;
  bool no_video_device = false;
  bool no_audio_device = false;
  std::string video_device = "";
  std::string resolution = "VGA";
  int framerate = 30;
  bool multistream = false;
  int width = 640;
  int height = 480;
  bool show_me = false;
  bool fullscreen = false;
  bool disable_echo_cancellation = false;
  bool disable_auto_gain_control = false;
  bool disable_noise_suppression = false;
  bool disable_highpass_filter = false;
  bool disable_residual_echo_detector = false;

  struct Size {
    int width;
    int height;
  };
  Size GetSize() {
    if (resolution == "QVGA") {
      return {320, 240};
    } else if (resolution == "VGA") {
      return {640, 480};
    } else if (resolution == "HD") {
      return {1280, 720};
    } else if (resolution == "FHD") {
      return {1920, 1080};
    } else if (resolution == "4K") {
      return {3840, 2160};
    }

    // 128x96 みたいな感じのフォーマット
    auto pos = resolution.find('x');
    if (pos == std::string::npos) {
      return {16, 16};
    }
    auto width = std::atoi(resolution.substr(0, pos).c_str());
    auto height = std::atoi(resolution.substr(pos + 1).c_str());
    return {std::max(16, width), std::max(16, height)};
  }
};

class SDLSample : public std::enable_shared_from_this<SDLSample>,
                  public sora::SoraDefaultClient {
 public:
  SDLSample(SDLSampleConfig config)
      : sora::SoraDefaultClient(config), config_(config) {}

  void Run() {
    renderer_ = nullptr;
    if (config_.role != "sendonly" || config_.show_me) {
      renderer_.reset(new SDLRenderer(config_.width, config_.height, config_.fullscreen));
    }
    if (config_.role != "recvonly") {
      if (!config_.no_video_device) {
	auto size = config_.GetSize();
	sora::CameraDeviceCapturerConfig cam_config;
	cam_config.width = size.width;
	cam_config.height = size.height;
	cam_config.fps = config_.framerate;
	cam_config.device_name = config_.video_device;
	auto video_source = sora::CreateCameraDeviceCapturer(cam_config);

	video_track_ = factory()->CreateVideoTrack(GenerateRandomChars(), video_source);
	if (config_.show_me) {
	  renderer_->AddTrack(video_track_.get());
	}
      }
      if (!config_.no_audio_device) {
	cricket::AudioOptions ao;
	if (config_.disable_echo_cancellation)
	  ao.echo_cancellation = false;
	if (config_.disable_auto_gain_control)
	  ao.auto_gain_control = false;
	if (config_.disable_noise_suppression)
	  ao.noise_suppression = false;
	if (config_.disable_highpass_filter)
	  ao.highpass_filter = false;
	if (config_.disable_residual_echo_detector)
	  ao.residual_echo_detector = false;
	RTC_LOG(LS_INFO) << __FUNCTION__ << ": " << ao.ToString();
	audio_track_ = factory()->CreateAudioTrack(
	   GenerateRandomChars(),
	   factory()->CreateAudioSource(cricket::AudioOptions()));
	if (!audio_track_) {
	  RTC_LOG(LS_WARNING) << __FUNCTION__ << ": Cannot create audio_track";
	}
      }
    }

    ioc_.reset(new boost::asio::io_context(1));

    sora::SoraSignalingConfig config;
    config.pc_factory = factory();
    config.io_context = ioc_.get();
    config.observer = shared_from_this();
    config.signaling_urls.push_back(config_.signaling_url);
    config.channel_id = config_.channel_id;
    config.multistream = config_.multistream;
    config.role = config_.role;
    config.video_codec_type = config_.video_codec_type;

    // メタデータのパース
    if (!config_.metadata.empty()) {
      config.metadata = boost::json::parse(config_.metadata);
    }

    conn_ = sora::SoraSignaling::Create(config);

    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        work_guard(ioc_->get_executor());

    boost::asio::signal_set signals(*ioc_, SIGINT, SIGTERM);
    signals.async_wait(
        [this](const boost::system::error_code&, int) { conn_->Disconnect(); });

    conn_->Connect();

    if (renderer_) {
      renderer_->SetDispatchFunction([this](std::function<void()> f) {
	if (ioc_->stopped())
	  return;
	boost::asio::dispatch(ioc_->get_executor(), f);
      });
      ioc_->run();
      renderer_->SetDispatchFunction(nullptr);
    } else {
      ioc_->run();
    }
  }

  void OnSetOffer() override {
    std::string stream_id = GenerateRandomChars();
    if (audio_track_ != nullptr) {
      webrtc::RTCErrorOr<rtc::scoped_refptr<webrtc::RtpSenderInterface>>
          audio_result =
              conn_->GetPeerConnection()->AddTrack(audio_track_, {stream_id});
    }
    if (video_track_ != nullptr) {
      webrtc::RTCErrorOr<rtc::scoped_refptr<webrtc::RtpSenderInterface>>
          video_result =
              conn_->GetPeerConnection()->AddTrack(video_track_, {stream_id});
    }
  }
  void OnDisconnect(sora::SoraSignalingErrorCode ec,
                    std::string message) override {
    RTC_LOG(LS_INFO) << "OnDisconnect: " << message;
    if (renderer_) {
      renderer_.reset();
    }
    ioc_->stop();
  }

  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {
    auto track = transceiver->receiver()->track();
    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
      if (renderer_) {
	renderer_->AddTrack(static_cast<webrtc::VideoTrackInterface*>(track.get()));
      }
    }
  }
  void OnRemoveTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {
    auto track = receiver->track();
    if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
      if (renderer_ ) {
	renderer_->RemoveTrack(static_cast<webrtc::VideoTrackInterface*>(track.get()));
      }
    }
  }

 private:
  SDLSampleConfig config_;
  rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
  rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
  std::shared_ptr<sora::SoraSignaling> conn_;
  std::unique_ptr<boost::asio::io_context> ioc_;
  std::unique_ptr<SDLRenderer> renderer_;
};

int main(int argc, char* argv[]) {
#ifdef _WIN32
  webrtc::ScopedCOMInitializer com_initializer(
      webrtc::ScopedCOMInitializer::kMTA);
  if (!com_initializer.Succeeded()) {
    std::cerr << "CoInitializeEx failed" << std::endl;
    return 1;
  }
#endif

  SDLSampleConfig config;

  auto bool_map = std::vector<std::pair<std::string, bool>>(
      {{"false", false}, {"true", true}});

  CLI::App app("SDL Sample for Sora C++ SDK");
  app.set_help_all_flag("--help-all",
                        "Print help message for all modes and exit");

  int log_level = (int)rtc::LS_ERROR;
  auto log_level_map = std::vector<std::pair<std::string, int>>(
      {{"verbose", 0}, {"info", 1}, {"warning", 2}, {"error", 3}, {"none", 4}});
  app.add_option("--log-level", log_level, "Log severity level threshold")
      ->transform(CLI::CheckedTransformer(log_level_map, CLI::ignore_case));
  app.add_flag("--no-video-device", config.no_video_device,
               "Do not use video device");
  app.add_flag("--no-audio-device", config.no_audio_device,
               "Do not use audio device");
  app.add_option("--video-device", config.video_device,
                 "Use the video input device specified by a name "
                 "(some device will be used if not specified)")
      ->check(CLI::ExistingFile);
  auto is_valid_resolution = CLI::Validator(
      [](std::string input) -> std::string {
        if (input == "QVGA" || input == "VGA" || input == "HD" ||
            input == "FHD" || input == "4K") {
          return std::string();
        }

        // 数値x数値、というフォーマットになっているか確認する
        std::regex re("^[1-9][0-9]*x[1-9][0-9]*$");
        if (std::regex_match(input, re)) {
          return std::string();
        }

        return "Must be one of QVGA, VGA, HD, FHD, 4K, or "
               "[WIDTH]x[HEIGHT].";
      },
      "");
  app.add_option("--resolution", config.resolution,
                 "Video resolution (one of QVGA, VGA, HD, FHD, 4K, or "
                 "[WIDTH]x[HEIGHT])")
      ->check(is_valid_resolution);
  app.add_option("--framerate", config.framerate, "Video framerate")
      ->check(CLI::Range(1, 60));

  // オーディオフラグ
  app.add_flag("--disable-echo-cancellation", config.disable_echo_cancellation,
               "Disable echo cancellation for audio");
  app.add_flag("--disable-auto-gain-control", config.disable_auto_gain_control,
               "Disable auto gain control for audio");
  app.add_flag("--disable-noise-suppression", config.disable_noise_suppression,
               "Disable noise suppression for audio");
  app.add_flag("--disable-highpass-filter", config.disable_highpass_filter,
               "Disable highpass filter for audio");
  app.add_flag("--disable-residual-echo-detector",
               config.disable_residual_echo_detector,
               "Disable residual echo detector for audio");

  // Sora に関するオプション
  app.add_option("--signaling-url", config.signaling_url, "Signaling URL")
      ->required();
  app.add_option("--channel-id", config.channel_id, "Channel ID")->required();
  app.add_option("--role", config.role, "Role")
      ->check(CLI::IsMember({"sendonly", "recvonly", "sendrecv"}))
      ->required();
  app.add_option("--video-codec-type", config.video_codec_type,
                 "Video codec for send")
      ->check(CLI::IsMember({"", "VP8", "VP9", "AV1", "H264"}));
  app.add_option("--multistream", config.multistream,
                 "Use multistream (default: false)")
      ->transform(CLI::CheckedTransformer(bool_map, CLI::ignore_case));
  auto is_json = CLI::Validator(
      [](std::string input) -> std::string {
        boost::json::error_code ec;
        boost::json::parse(input);
        if (ec) {
          return "Value " + input + " is not JSON Value";
        }
        return std::string();
      },
      "JSON Value");
  app.add_option("--metadata", config.metadata, "Signaling metadata used in connect message")
    ->check(is_json);

  // SDL に関するオプション
  app.add_option("--width", config.width, "SDL window width");
  app.add_option("--height", config.width, "SDL window height");
  app.add_flag("--fullscreen", config.fullscreen);
  app.add_flag("--show-me", config.show_me);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    exit(app.exit(e));
  }

  if (log_level != rtc::LS_NONE) {
    rtc::LogMessage::LogToDebug((rtc::LoggingSeverity)log_level);
    rtc::LogMessage::LogTimestamps();
    rtc::LogMessage::LogThreads();
  }

  config.use_audio_deivce = !config.no_audio_device;
  auto sdlsample = sora::CreateSoraClient<SDLSample>(config);
  sdlsample->Run();

  return 0;
}
