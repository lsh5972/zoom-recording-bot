#pragma once

#include "zoom_bot/capture_runtime.hpp"
#include "zoom_bot/join_config.hpp"
#include "sdk_listeners.hpp"
#include <deque>
#include <functional>
#include <set>

namespace zoom_bot {
class ZoomSession final : public SdkEventSink, public IZoomSDKAudioRawDataDelegate {
 public:
  ZoomSession(JoinConfig config, CaptureRuntime& capture);
  ~ZoomSession();
  void start();
  void tick();  // SDK actions run on the GLib/SDK main thread, never on the capture consumer.
  void stop(const std::string& reason);
  bool done() const { return done_; }
  bool failed_session() const { return error_ || capture_.failed(); }
  void record(const char* interface, const char* callback, nlohmann::json data) override;
  void failed() noexcept override;
  void onMixedAudioRawDataReceived(AudioRawData* data) override;
  void onOneWayAudioRawDataReceived(AudioRawData* data, uint32_t user_id) override;
  void onShareAudioRawDataReceived(AudioRawData* data, uint32_t user_id) override;
  void onOneWayInterpreterAudioRawDataReceived(AudioRawData* data, const zchar_t* language) override;

 private:
  void defer(std::function<void()> action);
  void check(SDKError result, const char* operation);
  void join();
  void status(MeetingStatus status, int result);
  void register_events(bool required = false);
  void snapshot_participants();
  void users(IList<unsigned int>* ids, bool joined);
  void try_recording();
  void pause_recording(const std::string& reason);
  void error(const char* reason);
  void raw_event(const char* kind, AudioRawData* data, nlohmann::json fields);

  JoinConfig config_;
  CaptureRuntime& capture_;
  IAuthService* auth_ = nullptr;
  IMeetingService* meeting_ = nullptr;
  ISettingService* settings_ = nullptr;
  INetworkConnectionHelper* network_ = nullptr;
  IMeetingParticipantsController* participants_ = nullptr;
  IMeetingRecordingController* recording_ = nullptr;
  IMeetingAudioController* audio_ = nullptr;
  IZoomSDKAudioRawDataHelper* raw_ = nullptr;
  bool initialized_ = false, in_meeting_ = false, recording_started_ = false, subscribed_ = false;
  bool join_requested_ = false, done_ = false, error_ = false;
  std::atomic<bool> denied_{false};
  std::atomic<bool> receiving_{false};
  std::chrono::steady_clock::time_point deadline_, next_permission_check_;
  std::mutex actions_mutex_, users_mutex_, raw_mutex_;
  std::deque<std::function<void()>> actions_;
  std::set<uint32_t> known_users_;
  nlohmann::json previous_permission_;

  IAuthServiceEventListener auth_events_{*this};
  IMeetingServiceEventListener meeting_events_{*this};
  IMeetingParticipantsCtrlEventListener participant_events_{*this};
  IMeetingRecordingCtrlEventListener recording_events_{*this};
  IMeetingAudioCtrlEventListener audio_events_{*this};
  IMeetingVideoCtrlEventListener video_events_{*this};
  IMeetingShareCtrlEventListener share_events_{*this};
  IMeetingChatCtrlEventListener chat_events_{*this};
  IMeetingWaitingRoomEventListener waiting_events_{*this};
  IMeetingReminderEventListener reminder_events_{*this};
  IMeetingConfigurationEventListener config_events_{*this};
  IMeetingBOControllerEventListener breakout_events_{*this};
  IBOCreatorEventListener bo_creator_events_{*this};
  IBOAdminEventListener bo_admin_events_{*this};
  IBOAttendeeEventListener bo_attendee_events_{*this};
  IBODataEventListener bo_data_events_{*this};
  IMeetingWebinarCtrlEventListener webinar_events_{*this};
  IMeetingEncryptionControllerEventListener encryption_events_{*this};
  IMeetingAICompanionCtrlEventListener ai_events_{*this};
  IMeetingAICompanionQueryHelperEventListener query_events_{*this};
  IMeetingAICompanionSmartSummaryHelperEventListener summary_events_{*this};
  IAudioSettingContextEventListener device_events_{*this};
  INetworkConnectionHandlerListener network_events_{*this};
};
}  // namespace zoom_bot
