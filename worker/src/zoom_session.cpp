#include "zoom_bot/zoom_session.hpp"

#include <stdexcept>

namespace zoom_bot {
namespace {
std::string name(IUserInfo* user) { return user && user->GetUserName() ? user->GetUserName() : ""; }
nlohmann::json user_snapshot(IUserInfo* user) {
  if (!user) return nullptr;
  return {{"user_id", user->GetUserID()}, {"display_name", name(user)},
          {"persistent_id", sdk_value(user->GetPersistentId())}, {"role", sdk_value(user->GetUserRole())},
          {"is_host", user->IsHost()}, {"is_self", user->IsMySelf()}, {"is_bot", user->IsBotUser()},
          {"audio_muted", user->IsAudioMuted()}, {"video_on", user->IsVideoOn()},
          {"in_waiting_room", user->IsInWaitingRoom()}};
}
}

ZoomSession::ZoomSession(JoinConfig config, CaptureRuntime& capture)
    : config_(std::move(config)), capture_(capture), share_(capture, config_.screenshot_interval_seconds) {
  auth_events_.after_onAuthenticationReturn = [this](AuthResult result) {
    defer([this, result] { if (result == AUTHRET_SUCCESS) join(); else error("sdk_auth_failed"); });
  };
  auth_events_.after_onZoomIdentityExpired = [this] { defer([this] { error("zoom_identity_expired"); }); };
  auth_events_.after_onZoomAuthIdentityExpired = [this] { defer([this] { error("sdk_identity_expired"); }); };
  network_events_.after_onProxySettingNotification = [this](IProxySettingHandler* handler) {
    if (handler) handler->Cancel();
    defer([this] { error("proxy_credentials_required"); });
  };
  network_events_.after_onSSLCertVerifyNotification = [this](ISSLCertVerificationHandler* handler) {
    if (handler) handler->Cancel();
    defer([this] { error("untrusted_ssl_certificate"); });
  };
  meeting_events_.after_onMeetingStatusChanged = [this](MeetingStatus state, int result) {
    if (state == MEETING_STATUS_INMEETING) capture_.admitted();
    // Stop accepting PCM immediately; cleanup and SDK calls wait for the main thread.
    if (state == MEETING_STATUS_RECONNECTING || state == MEETING_STATUS_ENDED || state == MEETING_STATUS_FAILED) {
      receiving_ = false;
      share_.suspend();
    }
    defer([this, state, result] { status(state, result); });
  };
  participant_events_.after_onUserJoin = [this](IList<unsigned int>* ids, const zchar_t*) { users(ids, true); };
  participant_events_.after_onUserLeft = [this](IList<unsigned int>* ids, const zchar_t*) { users(ids, false); };
  participant_events_.after_onUserNamesChanged = [this](IList<unsigned int>* ids) {
    if (ids && participants_) for (int i = 0; i < ids->GetCount(); ++i) {
      auto* user = participants_->GetUserByUserID(ids->GetItem(i));
      capture_.event("participant.updated", user_snapshot(user));
      if (user) capture_.renamed(user->GetUserID(), name(user));
    }
  };
  recording_events_.after_onCloudRecordingStatus = [this](RecordingStatus status) {
    capture_.cloud_recording(static_cast<int>(status));
  };
  participant_events_.after_onHostChangeNotification = [this](unsigned int) { defer([this] { try_recording(); }); };
  participant_events_.after_onCoHostChangeNotification = [this](unsigned int, bool) { defer([this] { try_recording(); }); };
  recording_events_.after_onRecordPrivilegeChanged = [this](bool allowed) {
    denied_ = !allowed;
    if (!allowed) { receiving_ = false; share_.suspend(); }
    defer([this] { try_recording(); });
  };
  recording_events_.after_onLocalRecordingPrivilegeRequestStatus = [this](RequestLocalRecordingStatus status) {
    if (status == RequestLocalRecording_Granted) denied_ = false;
    if (status == RequestLocalRecording_Denied) { denied_ = true; receiving_ = false; share_.suspend(); }
    defer([this] { try_recording(); });
  };
  audio_events_.after_onUserAudioStatusChange = [this](IList<IUserAudioStatus*>*, const zchar_t*) {
    defer([this] {
      auto* self = participants_ ? participants_->GetMySelfUser() : nullptr;
      if (self && audio_ && self->GetUserID() != 0 && self->GetAudioJoinType() != AUDIOTYPE_NONE && !self->IsAudioMuted())
        check(audio_->MuteAudio(self->GetUserID()), "mute_self");
      try_recording();
    });
  };
  video_events_.after_onHostRequestStartVideo = [this](IRequestStartVideoHandler* handler) {
    if (handler) check(handler->Ignore(), "ignore_video_request");
  };
  auto share_changed = [this](ZoomSDKSharingSourceInfo info) {
    // Copy only values; SDK-owned monitorID strings expire after the callback.
    defer([this, user = info.userid, source = info.shareSourceID, status = info.status, type = info.contentType] {
      if (subscribed_ && receiving_) share_.changed(user, source, status, type);
    });
  };
  share_events_.after_onSharingStatus = share_changed;
  share_events_.after_onShareContentNotification = share_changed;
  audio_events_.after_onHostRequestStartAudio = [this](IRequestStartAudioHandler* handler) {
    if (handler) check(handler->Ignore(), "ignore_unmute_request");
  };
  reminder_events_.after_onReminderNotify = [this](IMeetingReminderContent* content, IMeetingReminderHandler* handler) {
    if (!content || !handler) return;
    // Routine join/record notices belong to the requested recording flow. Legal/AI consent does not.
    const auto type = content->GetType();
    auto routine = [](MeetingReminderType type) {
      return type == TYPE_START_OR_JOIN_MEETING || type == TYPE_RECORD_REMINDER || type == TYPE_RECORD_DISCLAIMER;
    };
    bool accept = routine(type);
    if (type == TYPE_MULTI_DISCLAIMER) {
      auto* types = content->GetMultiReminderTypes();
      accept = types && types->GetCount() > 0;
      if (types) for (int i = 0; i < types->GetCount(); ++i) accept = accept && routine(types->GetItem(i));
    }
    if (accept)
      check(handler->Accept(), "accept_recording_notice");
    else if (content->IsBlocking()) {
      handler->Decline();
      defer([this] { error("blocking_reminder_requires_host_action"); });
    } else check(handler->Ignore(), "ignore_reminder");
  };
  reminder_events_.after_onEnableReminderNotify = [](IMeetingReminderContent*, IMeetingEnableReminderHandler* handler) {
    if (handler) handler->Ignore();
  };
  config_events_.after_onInputMeetingPasswordAndScreenNameNotification = [this](IMeetingPasswordAndScreenNameHandler* handler) {
    if (!handler) return;
    handler->Cancel();
    defer([this] { error("join_requires_additional_input"); });
  };
  config_events_.after_onEndOtherMeetingToJoinMeetingNotification = [this](IEndOtherMeetingToJoinMeetingHandler* handler) {
    if (handler) handler->Cancel();
    defer([this] { error("host_has_another_meeting"); });
  };
  config_events_.after_onJoinMeetingNeedUserInfo = [this](IMeetingInputUserInfoHandler* handler) {
    if (handler) handler->Cancel();
    defer([this] { error("join_requires_user_info"); });
  };
  breakout_events_.after_onHasCreatorRightsNotification = [this](IBOCreator* helper) {
    if (helper) helper->SetEvent(&bo_creator_events_);
  };
  breakout_events_.after_onHasAdminRightsNotification = [this](IBOAdmin* helper) {
    if (helper) helper->SetEvent(&bo_admin_events_);
  };
  breakout_events_.after_onHasAttendeeRightsNotification = [this](IBOAttendee* helper) {
    if (helper) helper->SetEvent(&bo_attendee_events_);
  };
  breakout_events_.after_onHasDataHelperRightsNotification = [this](IBOData* helper) {
    if (helper) helper->SetEvent(&bo_data_events_);
  };
}

ZoomSession::~ZoomSession() {
  stop("worker_stopped");
  if (meeting_) DestroyMeetingService(meeting_);
  if (auth_) DestroyAuthService(auth_);
  if (settings_) DestroySettingService(settings_);
  if (network_) DestroyNetworkConnectionHelper(network_);
  if (initialized_) CleanUPSDK();
}

void ZoomSession::record(const char* interface, const char* callback, nlohmann::json data) {
  capture_.event("sdk.callback", {{"interface", interface}, {"callback", callback}, {"arguments", std::move(data)}});
}
void ZoomSession::failed() noexcept { capture_.fail(); }

void ZoomSession::defer(std::function<void()> action) {
  std::lock_guard<std::mutex> lock(actions_mutex_);
  if (actions_.size() >= 128) { failed(); return; }
  actions_.push_back(std::move(action));
}

void ZoomSession::check(SDKError result, const char* operation) {
  capture_.event("sdk.operation", {{"operation", operation}, {"result", static_cast<int>(result)}});
  if (result != SDKERR_SUCCESS) throw std::runtime_error(std::string("SDK operation failed: ") + operation);
}

void ZoomSession::start() {
  deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  next_permission_check_ = std::chrono::steady_clock::now();
  InitParam init;
  init.strWebDomain = "https://zoom.us";
  init.emLanguageID = LANGUAGE_English;
  init.enableLogByDefault = false;
  init.enableGenerateDump = false;
  init.rawdataOpts.audioRawdataMemoryMode = ZoomSDKRawDataMemoryModeStack;
  check(InitSDK(init), "initialize");
  initialized_ = true;
  capture_.event("sdk.initialized", {{"version", sdk_value(GetSDKVersion())}, {"meeting_id", config_.meeting_id}});
  capture_.event("sdk.callback_inventory", sdk_callback_inventory());
  const auto network_result = CreateNetworkConnectionHelper(&network_);
  if (network_result == SDKERR_SUCCESS && network_)
    check(network_->RegisterNetworkConnectionHandler(&network_events_), "register_network_events");
  capture_.event("sdk.event_coverage", {{"interface", "INetworkConnectionHandler"},
                                       {"callbacks", network_events_.callbacks()},
                                       {"available", network_ != nullptr}, {"result", sdk_value(network_result)}});
  check(CreateAuthService(&auth_), "create_auth");
  check(CreateMeetingService(&meeting_), "create_meeting");
  check(CreateSettingService(&settings_), "create_settings");
  if (!auth_ || !meeting_ || !settings_) throw std::runtime_error("SDK did not create services");
  check(auth_->SetEvent(&auth_events_), "register_auth_events");
  check(meeting_->SetEvent(&meeting_events_), "register_meeting_events");
  AuthContext context;
  context.jwt_token = config_.sdk_jwt.c_str();
  check(auth_->SDKAuth(context), "authenticate");
}

void ZoomSession::join() {
  if (join_requested_ || done_) return;
  // The real SDK exposes audio settings only after successful authentication.
  auto* audio_settings = settings_->GetAudioSettings();
  if (!audio_settings) throw std::runtime_error("SDK audio settings unavailable");
  check(audio_settings->EnableAutoJoinAudio(false), "disable_automatic_audio_join");
  check(audio_settings->EnableAlwaysMuteMicWhenJoinVoip(true), "mute_on_join");
  check(audio_settings->SetAudioDeviceEvent(&device_events_), "register_audio_devices");
  capture_.event("sdk.event_coverage", {{"interface", "IAudioSettingContextEvent"},
                                       {"callbacks", device_events_.callbacks()}, {"registered", true}});
  register_events();
  JoinParam params;
  params.userType = SDK_UT_WITHOUT_LOGIN;
  auto& user = params.param.withoutloginuserJoin;
  user.meetingNumber = std::stoull(config_.meeting_id);
  user.userName = config_.display_name.c_str();
  user.psw = config_.passcode.c_str();
  user.userZAK = config_.user_zak.c_str();
  user.isVideoOff = true;
  user.isAudioOff = false;
  user.isAudioRawDataStereo = false;
  user.eAudioRawdataSamplingRate = AudioRawdataSamplingRate_32K;
  join_requested_ = true;
  check(meeting_->Join(params), "join_with_user_zak");
}

void ZoomSession::register_events(bool required) {
  participants_ = meeting_->GetMeetingParticipantsController();
  recording_ = meeting_->GetMeetingRecordingController();
  audio_ = meeting_->GetMeetingAudioController();
  if (required && (!participants_ || !recording_ || !audio_)) throw std::runtime_error("Required SDK controllers unavailable");
  if (participants_) check(participants_->SetEvent(&participant_events_), "register_participants");
  if (recording_) check(recording_->SetEvent(&recording_events_), "register_recording");
  if (audio_) check(audio_->SetEvent(&audio_events_), "register_audio");
  capture_.event("sdk.event_coverage", {{"interface", "IAuthServiceEvent"}, {"callbacks", auth_events_.callbacks()}, {"registered", true}});
  capture_.event("sdk.event_coverage", {{"interface", "IMeetingServiceEvent"}, {"callbacks", meeting_events_.callbacks()}, {"registered", true}});
  capture_.event("sdk.event_coverage", {{"interface", "IMeetingParticipantsCtrlEvent"}, {"callbacks", participant_events_.callbacks()}, {"available", participants_ != nullptr}});
  capture_.event("sdk.event_coverage", {{"interface", "IMeetingRecordingCtrlEvent"}, {"callbacks", recording_events_.callbacks()}, {"available", recording_ != nullptr}});
  capture_.event("sdk.event_coverage", {{"interface", "IMeetingAudioCtrlEvent"}, {"callbacks", audio_events_.callbacks()}, {"available", audio_ != nullptr}});
  auto attach = [this](auto* controller, auto& listener, const char* interface) {
    auto data = nlohmann::json{{"interface", interface}, {"callbacks", listener.callbacks()}, {"available", controller != nullptr}};
    if (controller) {
      using Result = decltype(controller->SetEvent(&listener));
      if constexpr (std::is_void_v<Result>) { controller->SetEvent(&listener); data["registered"] = true; }
      else {
        const auto result = controller->SetEvent(&listener);
        if constexpr (std::is_same_v<Result, bool>) data["registered"] = result;
        else { data["result"] = static_cast<int>(result); data["registered"] = result == SDKERR_SUCCESS; }
      }
    }
    capture_.event("sdk.event_coverage", std::move(data));
  };
  attach(meeting_->GetMeetingVideoController(), video_events_, "IMeetingVideoCtrlEvent");
  attach(meeting_->GetMeetingShareController(), share_events_, "IMeetingShareCtrlEvent");
  attach(meeting_->GetMeetingChatController(), chat_events_, "IMeetingChatCtrlEvent");
  attach(meeting_->GetMeetingWaitingRoomController(), waiting_events_, "IMeetingWaitingRoomEvent");
  attach(meeting_->GetMeetingReminderController(), reminder_events_, "IMeetingReminderEvent");
  attach(meeting_->GetMeetingConfiguration(), config_events_, "IMeetingConfigurationEvent");
  attach(meeting_->GetMeetingWebinarController(), webinar_events_, "IMeetingWebinarCtrlEvent");
  attach(meeting_->GetInMeetingEncryptionController(), encryption_events_, "IMeetingEncryptionControllerEvent");
  auto* breakout = meeting_->GetMeetingBOController();
  attach(breakout, breakout_events_, "IMeetingBOControllerEvent");
  attach(breakout ? breakout->GetBOCreatorHelper() : nullptr, bo_creator_events_, "IBOCreatorEvent");
  attach(breakout ? breakout->GetBOAdminHelper() : nullptr, bo_admin_events_, "IBOAdminEvent");
  attach(breakout ? breakout->GetBOAttedeeHelper() : nullptr, bo_attendee_events_, "IBOAttendeeEvent");
  attach(breakout ? breakout->GetBODataHelper() : nullptr, bo_data_events_, "IBODataEvent");
  auto* ai = meeting_->GetMeetingAICompanionController();
  attach(ai, ai_events_, "IMeetingAICompanionCtrlEvent");
  attach(ai ? ai->GetMeetingAICompanionQueryHelper() : nullptr, query_events_, "IMeetingAICompanionQueryHelperEvent");
  attach(ai ? ai->GetMeetingAICompanionSmartSummaryHelper() : nullptr, summary_events_, "IMeetingAICompanionSmartSummaryHelperEvent");
}

void ZoomSession::users(IList<unsigned int>* ids, bool joined) {
  if (!ids || !participants_) return;
  std::lock_guard<std::mutex> lock(users_mutex_);
  for (int i = 0; i < ids->GetCount(); ++i) {
    const auto id = ids->GetItem(i);
    if (joined) {
      auto* user = participants_->GetUserByUserID(id);
      capture_.event("participant.snapshot", user_snapshot(user));
      if (known_users_.insert(id).second) capture_.joined(id, name(user));
      else if (user && !name(user).empty()) capture_.renamed(id, name(user));
    } else if (known_users_.erase(id)) capture_.left(id);
  }
}

void ZoomSession::snapshot_participants() {
  auto* list = participants_->GetParticipantsList();
  if (!list) return;
  users(list, true);
  std::set<uint32_t> present;
  for (int i = 0; i < list->GetCount(); ++i) present.insert(list->GetItem(i));
  if (auto* self = participants_->GetMySelfUser()) present.insert(self->GetUserID());
  std::lock_guard<std::mutex> lock(users_mutex_);
  for (auto it = known_users_.begin(); it != known_users_.end();) {
    if (present.count(*it)) { ++it; continue; }
    capture_.left(*it);  // Reconcile departures missed during a transport reconnect.
    it = known_users_.erase(it);
  }
}

void ZoomSession::status(MeetingStatus state, int result) {
  if (done_) return;
  if (state == MEETING_STATUS_INMEETING) {
    in_meeting_ = true;
    register_events(true);
    snapshot_participants();
    capture_.cloud_recording(static_cast<int>(recording_->GetCloudRecordingStatus()));
    capture_.event("meeting.admitted", {{"self", user_snapshot(participants_->GetMySelfUser())}});
    auto* self = participants_->GetMySelfUser();
    if (!self || self->GetAudioJoinType() == AUDIOTYPE_NONE) check(audio_->JoinVoip(), "join_voip");
    if (self && self->GetUserID() != 0 && self->GetAudioJoinType() != AUDIOTYPE_NONE && !self->IsAudioMuted())
      check(audio_->MuteAudio(self->GetUserID()), "mute_self");
    try_recording();
  } else if (state == MEETING_STATUS_RECONNECTING) {
    in_meeting_ = false;
    pause_recording("reconnecting");
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  } else if (state == MEETING_STATUS_FAILED) {
    capture_.event("meeting.failed", {{"result", result}});
    error("meeting_join_failed");
  } else if (state == MEETING_STATUS_ENDED || (state == MEETING_STATUS_IDLE && join_requested_)) {
    in_meeting_ = false;
    stop("meeting_ended");
  }
}

void ZoomSession::try_recording() {
  if (!in_meeting_ || done_) return;
  auto* self = participants_->GetMySelfUser();
  const auto role = self ? self->GetUserRole() : USERROLE_NONE;
  // CanStartRawRecording checks starting authority. During capture, revocation comes from the SDK event.
  const auto permission = subscribed_ ? SDKERR_SUCCESS : recording_->CanStartRawRecording();
  const auto local_permission = self ? recording_->CanStartRecording(false, self->GetUserID()) : SDKERR_NO_PERMISSION;
  nlohmann::json state = {{"role", sdk_value(role)}, {"is_host", self && self->IsHost()},
                          {"role_name", role == USERROLE_HOST ? "host" : role == USERROLE_COHOST ? "cohost" : "other"},
                          {"audio_join_type", self ? sdk_value(self->GetAudioJoinType()) : nlohmann::json(nullptr)},
                          {"local_recording_result", sdk_value(local_permission)},
                          {"local_recording_support_result", self ? sdk_value(recording_->IsSupportLocalRecording(self->GetUserID())) : nlohmann::json(nullptr)},
                          {"raw_recording_result", static_cast<int>(permission)}, {"privilege_revoked", denied_.load()},
                          {"permission_source", subscribed_ ? "active_subscription_and_privilege_events" : "can_start_raw_recording"}};
  if (state != previous_permission_) { capture_.event("recording.permission", state); previous_permission_ = state; }
  if (!self || denied_ || permission != SDKERR_SUCCESS) {
    if (recording_started_ || subscribed_) pause_recording("recording_permission_lost");
    if (self && !denied_ && permission == SDKERR_NO_PERMISSION &&
        role == USERROLE_ATTENDEE && !recording_privilege_requested_) {
      const auto supported = recording_->IsSupportRequestLocalRecordingPrivilege();
      if (supported == SDKERR_SUCCESS) {
        recording_privilege_requested_ = true;
        const auto requested = recording_->RequestLocalRecordingPrivilege();
        capture_.event("sdk.operation", {{"operation", "request_local_recording_privilege"},
                                         {"result", sdk_value(requested)}});
      }
    }
    return;
  }
  if (subscribed_) { share_.refresh(meeting_->GetMeetingShareController()); return; }
  raw_ = GetAudioRawdataHelper();
  if (!raw_) throw std::runtime_error("SDK raw audio helper unavailable");
  snapshot_participants();
  const auto started = recording_->StartRawRecording();
  capture_.event("sdk.operation", {{"operation", "start_raw_recording"}, {"result", sdk_value(started)}});
  if (started == SDKERR_NOT_JOIN_AUDIO || started == SDKERR_NEED_USER_CONFIRM_RECORD_DISCLAIMER) return;
  if (started != SDKERR_SUCCESS) throw std::runtime_error("SDK raw recording start failed");
  recording_started_ = true;
  receiving_ = !denied_;
  const auto subscribed = raw_->subscribe(this, false);
  if (subscribed != SDKERR_SUCCESS) { receiving_ = false; check(subscribed, "subscribe_one_way_audio"); }
  subscribed_ = true;
  share_.refresh(meeting_->GetMeetingShareController());
  capture_.event("sdk.event_coverage", {{"interface", "IZoomSDKAudioRawDataDelegate"}, {"registered", true},
                                       {"callbacks", {"onMixedAudioRawDataReceived", "onOneWayAudioRawDataReceived",
                                                      "onShareAudioRawDataReceived", "onOneWayInterpreterAudioRawDataReceived"}}});
  capture_.event("capture.started", {{"self_user_id", self->GetUserID()}, {"sample_rate_requested", 32000},
                                      {"channels_requested", 1}, {"timestamp_origin", "cloud_recording"}});
}

void ZoomSession::pause_recording(const std::string& reason) {
  { std::lock_guard<std::mutex> lock(raw_mutex_); receiving_ = false; }
  share_.stop();
  if (subscribed_ && raw_) {
    capture_.event("sdk.operation", {{"operation", "unsubscribe_audio"}, {"result", sdk_value(raw_->unSubscribe())}});
    subscribed_ = false;
  }
  if (recording_started_ && recording_) {
    capture_.event("sdk.operation", {{"operation", "stop_raw_recording"}, {"result", sdk_value(recording_->StopRawRecording())}});
    recording_started_ = false;
  }
  capture_.pause(reason);
  deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(120);
}

void ZoomSession::error(const char* reason) {
  capture_.event("worker.error", {{"reason", reason}});
  error_ = true;
  stop(reason);
}

void ZoomSession::stop(const std::string& reason) {
  if (done_) return;
  done_ = true;
  pause_recording(reason);
  if (meeting_ && join_requested_ && reason != "meeting_ended")
    capture_.event("sdk.operation", {{"operation", "leave_meeting"}, {"result", sdk_value(meeting_->Leave(LEAVE_MEETING))}});
  in_meeting_ = false;
  capture_.event("worker.stopped", {{"reason", reason}});
}

void ZoomSession::tick() {
  if (done_) return;
  if (capture_.failed()) { error("capture_failed"); return; }
  std::deque<std::function<void()>> actions;
  { std::lock_guard<std::mutex> lock(actions_mutex_); actions.swap(actions_); }
  try {
    for (auto& action : actions) { if (done_) break; action(); }
    const auto now = std::chrono::steady_clock::now();
    if (in_meeting_ && now >= next_permission_check_) {
      try_recording();
      next_permission_check_ = now + std::chrono::seconds(1);
    }
    if (subscribed_ && receiving_) share_.tick(capture_.now_ms());
    if (!subscribed_ && now >= deadline_) error("admission_or_recording_permission_timeout");
  } catch (...) { error("sdk_operation_failed"); }
}

void ZoomSession::raw_event(const char* kind, AudioRawData* data, nlohmann::json fields) {
  if (data) {
    fields["source_timestamp_ms"] = data->GetTimeStamp();
    fields["sample_rate"] = data->GetSampleRate();
    fields["channels"] = data->GetChannelNum();
    fields["bytes"] = data->GetBufferLen();
  }
  capture_.event(kind, std::move(fields));
}
void ZoomSession::onMixedAudioRawDataReceived(AudioRawData* data) {
  try { raw_event("sdk.raw_audio.mixed", data, {}); } catch (...) { failed(); }
}
void ZoomSession::onShareAudioRawDataReceived(AudioRawData* data, uint32_t user_id) {
  try { raw_event("sdk.raw_audio.share", data, {{"user_id", user_id}}); } catch (...) { failed(); }
}
void ZoomSession::onOneWayInterpreterAudioRawDataReceived(AudioRawData* data, const zchar_t* language) {
  try { raw_event("sdk.raw_audio.interpreter", data, {{"language", sdk_value(language)}}); } catch (...) { failed(); }
}
void ZoomSession::onOneWayAudioRawDataReceived(AudioRawData* data, uint32_t user_id) {
  try {
    std::lock_guard<std::mutex> lock(raw_mutex_);
    if (!receiving_) return;
    if (!data || !data->GetBuffer() || data->GetBufferLen() == 0 || data->GetBufferLen() % sizeof(int16_t) != 0)
      throw std::runtime_error("Invalid SDK PCM buffer");
    PcmPacket packet{user_id, static_cast<int>(data->GetSampleRate()), static_cast<int>(data->GetChannelNum()), 0, {}};
    packet.samples.resize(data->GetBufferLen() / sizeof(int16_t));
    std::memcpy(packet.samples.data(), data->GetBuffer(), data->GetBufferLen());
    capture_.pcm(std::move(packet), data->GetTimeStamp());
  } catch (...) { failed(); }
}
}  // namespace zoom_bot
