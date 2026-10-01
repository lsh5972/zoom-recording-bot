#include "zoom_bot/zoom_session.hpp"
#include "sdk_stubs.hpp"

#include <fstream>
#include <iostream>
#include <sndfile.h>
#include <stdexcept>
#include <unistd.h>

using namespace zoom_bot;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

template <typename T> struct List : IList<T> {
  std::vector<T> items;
  int GetCount() override { return static_cast<int>(items.size()); }
  T GetItem(int i) override { return items.at(i); }
  void AddItem(T item) override { items.push_back(item); }
};
struct User : IUserInfoStub {
  uint32_t id;
  UserRole role;
  std::string name;
  bool muted = true;
  AudioType audio_type = AUDIOTYPE_NONE;
  User(uint32_t id, UserRole role, std::string name) : id(id), role(role), name(std::move(name)) {}
  uint32_t GetUserID() override { return id; }
  const zchar_t* GetUserName() override { return name.c_str(); }
  UserRole GetUserRole() override { return role; }
  bool IsHost() override { return role == USERROLE_HOST; }
  bool IsMySelf() override { return id == 1; }
  bool IsAudioMuted() override { return muted; }
  AudioType GetAudioJoinType() override { return audio_type; }
};
struct Participants : IMeetingParticipantsControllerStub {
  IMeetingParticipantsCtrlEvent* listener = nullptr;
  User self{1, USERROLE_HOST, "Bot"}, alice{42, USERROLE_ATTENDEE, "Alice"}, bob{73, USERROLE_ATTENDEE, "Bob"};
  List<uint32_t> users;
  Participants() { users.items = {1, 42, 73}; }
  SDKError SetEvent(IMeetingParticipantsCtrlEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  IUserInfo* GetMySelfUser() override { return &self; }
  IList<uint32_t>* GetParticipantsList() override { return &users; }
  IUserInfo* GetUserByUserID(uint32_t id) override { return id == 1 ? &self : id == 42 ? &alice : &bob; }
};
struct Recording : IMeetingRecordingControllerStub {
  IMeetingRecordingCtrlEvent* listener = nullptr;
  SDKError permission = SDKERR_SUCCESS, start_result = SDKERR_SUCCESS;
  int starts = 0, stops = 0;
  SDKError SetEvent(IMeetingRecordingCtrlEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  SDKError CanStartRawRecording() override { return permission; }
  SDKError StartRawRecording() override { ++starts; return start_result; }
  SDKError StopRawRecording() override { ++stops; return SDKERR_SUCCESS; }
};
struct Audio : IMeetingAudioControllerStub {
  IMeetingAudioCtrlEvent* listener = nullptr;
  uint32_t muted_user = 0;
  SDKError SetEvent(IMeetingAudioCtrlEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  SDKError JoinVoip() override { return SDKERR_SUCCESS; }
  SDKError MuteAudio(uint32_t id, bool) override { muted_user = id; return SDKERR_SUCCESS; }
};
struct AudioSettings : IAudioSettingContextStub {
  bool automatic = true, muted = false;
  SDKError EnableAutoJoinAudio(bool value) override { automatic = value; return SDKERR_SUCCESS; }
  SDKError EnableAlwaysMuteMicWhenJoinVoip(bool value) override { muted = value; return SDKERR_SUCCESS; }
  SDKError SetAudioDeviceEvent(IAudioSettingContextEvent*) override { return SDKERR_SUCCESS; }
};
struct Settings : ISettingServiceStub {
  AudioSettings audio;
  int audio_requests = 0;
  IAudioSettingContext* GetAudioSettings() override { ++audio_requests; return &audio; }
};
struct Auth : IAuthServiceStub {
  IAuthServiceEvent* listener = nullptr;
  std::string jwt;
  SDKError SetEvent(IAuthServiceEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  SDKError SDKAuth(AuthContext& context) override { jwt = context.jwt_token; return SDKERR_SUCCESS; }
};
template <typename Signature> struct RawHelper;
template <typename... SessionArgs>
struct RawHelper<SDKError (IZoomSDKAudioRawDataHelper::*)(IZoomSDKAudioRawDataDelegate*, bool, SessionArgs...)>
    : IZoomSDKAudioRawDataHelperStub {
  IZoomSDKAudioRawDataDelegate* delegate = nullptr;
  SDKError result = SDKERR_SUCCESS;
  int subscriptions = 0, unsubscriptions = 0;
  SDKError subscribe(IZoomSDKAudioRawDataDelegate* value, bool, SessionArgs...) override {
    ++subscriptions;
    if (result == SDKERR_SUCCESS) delegate = value;
    return result;
  }
  SDKError unSubscribe(SessionArgs...) override { ++unsubscriptions; delegate = nullptr; return SDKERR_SUCCESS; }
};
using Raw = RawHelper<decltype(&IZoomSDKAudioRawDataHelper::subscribe)>;
struct Reminder : IMeetingReminderController {
  IMeetingReminderEvent* listener = nullptr;
  SDKError SetEvent(IMeetingReminderEvent* value) override { listener = value; return SDKERR_SUCCESS; }
};
struct ReminderContent : IMeetingReminderContent {
  List<MeetingReminderType> types;
  MeetingReminderType GetType() override { return TYPE_MULTI_DISCLAIMER; }
  const zchar_t* GetTitle() override { return "Recording notice"; }
  const zchar_t* GetContent() override { return "This meeting is being recorded."; }
  bool IsBlocking() override { return true; }
  ActionType GetActionType() override { return ACTION_TYPE_NONE; }
  IList<MeetingReminderType>* GetMultiReminderTypes() override { return &types; }
};
struct ReminderHandler : IMeetingReminderHandler {
  int accepted = 0, declined = 0;
  SDKError Ignore() override { return SDKERR_SUCCESS; }
  SDKError Accept() override { ++accepted; return SDKERR_SUCCESS; }
  SDKError Decline() override { ++declined; return SDKERR_SUCCESS; }
  SDKError SetHideFeatureDisclaimers() override { return SDKERR_SUCCESS; }
  bool IsNeedExplicitConsent4AICustomDisclaimer() override { return false; }
};
struct Meeting : IMeetingServiceStub {
  IMeetingServiceEvent* listener = nullptr;
  Participants participants;
  Recording recording;
  Audio audio;
  Reminder reminder;
  int joins = 0, starts = 0, leaves = 0;
  uint64_t number = 0;
  std::string zak, password;
  LeaveMeetingCmd leave_command = END_MEETING;
  SDKError SetEvent(IMeetingServiceEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  SDKError Join(JoinParam& params) override {
    require(params.userType == SDK_UT_WITHOUT_LOGIN, "Native join must use without-login user type");
    auto& user = params.param.withoutloginuserJoin;
    require(user.app_privilege_token == nullptr, "Host ZAK must not be passed as app privilege token");
    require(user.isVideoOff && !user.isAudioRawDataStereo, "Bot must join with video off and mono capture");
    number = user.meetingNumber; zak = user.userZAK; password = user.psw; ++joins;
    return SDKERR_SUCCESS;
  }
  SDKError Start(StartParam&) override { ++starts; return SDKERR_SUCCESS; }
  SDKError Leave(LeaveMeetingCmd command) override { ++leaves; leave_command = command; return SDKERR_SUCCESS; }
  IMeetingParticipantsController* GetMeetingParticipantsController() override { return &participants; }
  IMeetingRecordingController* GetMeetingRecordingController() override { return &recording; }
  IMeetingAudioController* GetMeetingAudioController() override { return &audio; }
  IMeetingReminderController* GetMeetingReminderController() override { return &reminder; }
};
struct Backend { Auth auth; Meeting meeting; Settings settings; Raw raw; int cleanup = 0; };
Backend* backend;

namespace ZOOMSDK {
extern "C" {
SDKError InitSDK(InitParam&) { return SDKERR_SUCCESS; }
SDKError CreateAuthService(IAuthService** out) { *out = &backend->auth; return SDKERR_SUCCESS; }
SDKError CreateMeetingService(IMeetingService** out) { *out = &backend->meeting; return SDKERR_SUCCESS; }
SDKError CreateSettingService(ISettingService** out) { *out = &backend->settings; return SDKERR_SUCCESS; }
SDKError CreateNetworkConnectionHelper(INetworkConnectionHelper** out) { *out = nullptr; return SDKERR_NO_IMPL; }
SDKError DestroyNetworkConnectionHelper(INetworkConnectionHelper*) { return SDKERR_SUCCESS; }
SDKError DestroyAuthService(IAuthService*) { return SDKERR_SUCCESS; }
SDKError DestroyMeetingService(IMeetingService*) { return SDKERR_SUCCESS; }
SDKError DestroySettingService(ISettingService*) { return SDKERR_SUCCESS; }
SDKError CleanUPSDK() { ++backend->cleanup; return SDKERR_SUCCESS; }
const zchar_t* GetSDKVersion() { return "test-double-public-api"; }
IZoomSDKAudioRawDataHelper* GetAudioRawdataHelper() { return &backend->raw; }
}
}

struct Output {
  std::filesystem::path root;
  Output() { char path[] = "/tmp/zoom-bot-sdk-XXXXXX"; root = mkdtemp(path); }
  ~Output() { std::filesystem::remove_all(root); }
  std::vector<Json> events(const std::string& type = "") {
    std::vector<Json> result;
    std::ifstream file(root / "events.jsonl");
    for (std::string line; std::getline(file, line);) {
      require(line.find("secret-zak") == std::string::npos && line.find("secret-jwt") == std::string::npos,
              "Events must not expose join credentials");
      auto event = Json::parse(line);
      if (type.empty() || event["type"] == type) result.push_back(std::move(event));
    }
    return result;
  }
};
JoinConfig config() { return {"00000000-0000-4000-8000-000000000001", "123456789", "host", "pass", "Recorder", "secret-jwt", "secret-zak"}; }
void admit(ZoomSession& session, Backend& sdk) {
  session.start();
  require(sdk.meeting.joins == 0, "Join must wait for SDK authentication callback");
  require(sdk.settings.audio_requests == 0, "Audio settings must wait for SDK authentication callback");
  sdk.auth.listener->onAuthenticationReturn(AUTHRET_SUCCESS);
  session.tick();
  require(sdk.settings.audio_requests == 1 && !sdk.settings.audio.automatic && sdk.settings.audio.muted,
          "Configure manual audio join and muted microphone after authentication, before meeting join");
  require(sdk.meeting.joins == 1 && sdk.meeting.starts == 0, "Bot must join the existing meeting, never start another");
  require(sdk.meeting.zak == "secret-zak" && sdk.meeting.number == 123456789, "Join must carry actual host ZAK and meeting ID");
  sdk.meeting.listener->onMeetingStatusChanged(MEETING_STATUS_INMEETING, 0);
  session.tick();
}
struct Packet : AudioRawDataStub {
  std::vector<int16_t> samples;
  uint64_t timestamp = 0;
  char* GetBuffer() override { return reinterpret_cast<char*>(samples.data()); }
  unsigned int GetBufferLen() override { return static_cast<unsigned int>(samples.size() * sizeof(int16_t)); }
  unsigned int GetSampleRate() override { return 16000; }
  unsigned int GetChannelNum() override { return 1; }
  unsigned long long GetTimeStamp() override { return timestamp; }
};

void rejected_auth_never_joins() {
  Backend sdk; backend = &sdk; Output output;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture);
    session.start();
    sdk.auth.listener->onAuthenticationReturn(AUTHRET_JWTTOKENWRONG);
    session.tick();
    require(session.done() && session.failed_session() && sdk.meeting.joins == 0, "Rejected auth must fail before joining");
  }
  capture.close(); output.events();
  require(sdk.cleanup == 1, "SDK must clean up after auth failure");
}

void role_and_permission_are_both_required() {
  for (int mode = 0; mode < 2; ++mode) {
    Backend sdk; backend = &sdk; Output output;
    if (mode == 0) sdk.meeting.participants.self.role = USERROLE_ATTENDEE;
    else sdk.meeting.recording.permission = SDKERR_NO_PERMISSION;
    CaptureRuntime capture(output.root, "session");
    {
      ZoomSession session(config(), capture); admit(session, sdk);
      require(sdk.meeting.recording.starts == 0 && sdk.raw.subscriptions == 0,
              "Host ZAK must not bypass actual role or raw recording permission");
      session.stop("test_stop");
    }
    capture.close();
    require(output.events("capture.started").empty(), "Denied recording must never report capture success");
  }
}

void revocation_stops_and_grant_resumes() {
  Backend sdk; backend = &sdk; Output output;
  sdk.meeting.participants.self.muted = false;
  sdk.meeting.participants.self.audio_type = AUDIOTYPE_VOIP;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture); admit(session, sdk);
    require(sdk.raw.subscriptions == 1, "Authorized host must subscribe to per-user raw audio");
    require(sdk.settings.audio.muted && sdk.meeting.audio.muted_user == 1, "Bot must mute only itself");
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(false); session.tick();
    require(sdk.raw.unsubscriptions == 1 && sdk.meeting.recording.stops == 1, "Revocation must stop recording and unsubscribe");
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(true); session.tick();
    require(sdk.raw.subscriptions == 2, "Explicit grant must resume capture");
    session.stop("signal");
    require(sdk.meeting.leaves == 1 && sdk.meeting.leave_command == LEAVE_MEETING, "Shutdown must leave only the bot");
  }
  capture.close();
  require(output.events("capture.started").size() == 2, "Each successful subscription needs a capture event");
}

void reconnect_and_role_loss_stop_capture() {
  Backend sdk; backend = &sdk; Output output;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture); admit(session, sdk);
    sdk.meeting.participants.self.role = USERROLE_ATTENDEE;
    sdk.meeting.participants.listener->onHostChangeNotification(42); session.tick();
    require(sdk.raw.unsubscriptions == 1, "Losing host/cohost role must stop collection");
    sdk.meeting.participants.self.role = USERROLE_COHOST;
    sdk.meeting.participants.listener->onCoHostChangeNotification(1, true); session.tick();
    require(sdk.raw.subscriptions == 2, "Actual cohost role with permission must resume collection");
    sdk.meeting.listener->onMeetingStatusChanged(MEETING_STATUS_RECONNECTING, 0); session.tick();
    require(sdk.raw.unsubscriptions == 2, "Reconnect must close the previous raw subscription");
    sdk.meeting.participants.users.items = {1, 42};  // Bob leaves while transport callbacks are unavailable.
    sdk.meeting.listener->onMeetingStatusChanged(MEETING_STATUS_INMEETING, 0); session.tick();
    require(sdk.raw.subscriptions == 3, "Re-admission must verify authority and re-subscribe");
    session.stop("test_stop");
  }
  capture.close();
  require(output.events("participant.joined").size() == 3, "Permission and transport pauses must preserve known speaker sessions");
  auto departed = output.events("participant.left");
  require(departed.size() == 1 && departed.front()["data"]["user_id"] == 73,
          "Re-admission must reconcile participants who left during reconnect");
}

void delayed_audio_and_subscription_failure() {
  for (bool rejected : {false, true}) {
    Backend sdk; backend = &sdk; Output output;
    sdk.meeting.recording.start_result = rejected ? SDKERR_SUCCESS : SDKERR_NOT_JOIN_AUDIO;
    sdk.raw.result = rejected ? SDKERR_NO_PERMISSION : SDKERR_SUCCESS;
    CaptureRuntime capture(output.root, "session");
    {
      ZoomSession session(config(), capture); admit(session, sdk);
      if (rejected) {
        require(session.failed_session() && sdk.meeting.recording.stops == 1,
                "Failed subscription must roll back raw recording");
      } else {
        require(!session.done() && sdk.raw.subscriptions == 0, "Pending VoIP join must wait rather than fail");
        sdk.meeting.recording.start_result = SDKERR_SUCCESS;
        sdk.meeting.audio.listener->onUserAudioStatusChange(nullptr, nullptr); session.tick();
        require(sdk.raw.subscriptions == 1, "Audio status callback must retry recording after VoIP joins");
      }
      session.stop("test_stop");
    }
    capture.close();
    if (rejected) require(output.events("capture.started").empty(), "Failed subscription must never report successful capture");
  }
}

void combined_recording_notices() {
  for (int scenario = 0; scenario < 3; ++scenario) {
    Backend sdk; backend = &sdk; Output output;
    CaptureRuntime capture(output.root, "session");
    {
      ZoomSession session(config(), capture); admit(session, sdk);
      ReminderContent content; ReminderHandler handler;
      if (scenario == 0) content.types.items = {TYPE_RECORD_REMINDER, TYPE_RECORD_DISCLAIMER};
      if (scenario == 1) content.types.items = {TYPE_RECORD_REMINDER, TYPE_QUERY_DISCLAIMER};
      sdk.meeting.reminder.listener->onReminderNotify(&content, &handler); session.tick();
      require(handler.accepted == (scenario == 0) && handler.declined == (scenario != 0),
              "Accept recording-only combinations; decline AI and unknown combinations");
      require(session.done() == (scenario != 0), "Blocking unsupported notices must stop capture");
      session.stop("test_stop");
    }
    capture.close();
    bool preserved = false;
    for (const auto& e : output.events("sdk.callback")) if (e["data"]["callback"] == "onReminderNotify")
      preserved = e["data"]["arguments"]["content"]["types"].size() == (scenario == 2 ? 0 : 2);
    require(preserved, "Copy the component types of a combined reminder into the journal");
  }
}

void real_pcm_fixture_through_sdk_callbacks() {
  SF_INFO info{};
  auto* file = sf_open(FVAD_FIXTURE, SFM_READ, &info);
  require(file != nullptr && info.samplerate == 16000, "Speech fixture must load");
  std::vector<int16_t> speech(info.frames);
  sf_readf_short(file, speech.data(), info.frames); sf_close(file);
  Backend sdk; backend = &sdk; Output output;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture); admit(session, sdk);
    List<uint32_t> active; active.items = {42, 73};
    sdk.meeting.audio.listener->onUserActiveAudioChange(&active);
    for (size_t offset = 0; offset < speech.size(); offset += 160) {
      Packet packet; packet.timestamp = 9000 + offset / 16;
      packet.samples.assign(speech.begin() + offset, speech.begin() + std::min(offset + 160, speech.size()));
      sdk.raw.delegate->onOneWayAudioRawDataReceived(&packet, 42);
      for (auto& sample : packet.samples) sample /= 2;
      sdk.raw.delegate->onOneWayAudioRawDataReceived(&packet, 73);
      std::fill(packet.samples.begin(), packet.samples.end(), 0);  // SDK buffer expires after each callback.
    }
    session.stop("signal");
  }
  capture.close();
  require(!capture.failed(), "Copied SDK buffers must remain valid on the consumer");
  const auto chunks = output.events("audio.chunk_ready");
  std::set<uint32_t> speakers;
  for (const auto& chunk : chunks) {
    speakers.insert(chunk["data"]["user_id"].get<uint32_t>());
    const auto path = chunk["data"]["wav_path"].get<std::string>();
    require(std::filesystem::path(path).parent_path().empty(), "Every speaker WAV must stay in one meeting folder");
    SF_INFO wav_info{};
    auto* wav = sf_open((output.root / path).c_str(), SFM_READ, &wav_info);
    require(wav != nullptr, "Published SDK WAV must be independently decodable");
    std::vector<int16_t> decoded(wav_info.frames); sf_readf_short(wav, decoded.data(), wav_info.frames); sf_close(wav);
    require(std::any_of(decoded.begin(), decoded.end(), [](int16_t sample) { return sample != 0; }),
            "WAV must contain copied speech even after SDK overwrites its callback buffer");
  }
  require(speakers == std::set<uint32_t>{42, 73}, "Simultaneous SDK speakers must produce separate WAVs");
  require(output.events("speech_on").size() >= 2 && output.events("speech_off").size() >= 2,
          "SDK capture must record per-speaker speech transitions");
  bool preserved = false;
  for (const auto& event : output.events("sdk.callback"))
    if (event["data"]["callback"] == "onUserActiveAudioChange")
      preserved = event["data"]["arguments"]["plstActiveAudio"] == Json::array({42, 73});
  require(preserved, "SDK active-audio list must be preserved separately from VAD speech events");
}

int main() {
  try {
    rejected_auth_never_joins(); role_and_permission_are_both_required();
    revocation_stops_and_grant_resumes(); reconnect_and_role_loss_stop_capture(); delayed_audio_and_subscription_failure();
    combined_recording_notices();
    real_pcm_fixture_through_sdk_callbacks();
    std::cout << "11 SDK adapter scenarios passed (test doubles, no live meeting)\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
