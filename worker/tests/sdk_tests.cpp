#include "zoom_bot/zoom_session.hpp"
#include "sdk_stubs.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <jpeglib.h>
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
  bool alice_available = true;
  List<uint32_t> users;
  Participants() { users.items = {1, 42, 73}; }
  SDKError SetEvent(IMeetingParticipantsCtrlEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  IUserInfo* GetMySelfUser() override { return &self; }
  IList<uint32_t>* GetParticipantsList() override { return &users; }
  IUserInfo* GetUserByUserID(uint32_t id) override { return id == 1 ? &self : id == 42 ? (alice_available ? &alice : nullptr) : &bob; }
};
struct Recording : IMeetingRecordingControllerStub {
  IMeetingRecordingCtrlEvent* listener = nullptr;
  SDKError permission = SDKERR_SUCCESS, start_result = SDKERR_SUCCESS;
  SDKError request_support = SDKERR_NO_PERMISSION, request_result = SDKERR_SUCCESS;
  int starts = 0, stops = 0, requests = 0;
  SDKError SetEvent(IMeetingRecordingCtrlEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  SDKError CanStartRawRecording() override { return permission; }
  SDKError StartRawRecording() override { ++starts; return start_result; }
  SDKError StopRawRecording() override { ++stops; return SDKERR_SUCCESS; }
  SDKError IsSupportRequestLocalRecordingPrivilege() override { return request_support; }
  SDKError RequestLocalRecordingPrivilege() override { ++requests; return request_result; }
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
struct ShareController : IMeetingShareControllerStub {
  IMeetingShareCtrlEvent* listener = nullptr;
  List<uint32_t> users;
  List<ZoomSDKSharingSourceInfo> sources;
  SDKError SetEvent(IMeetingShareCtrlEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  IList<uint32_t>* GetViewableSharingUserList() override { return &users; }
  IList<ZoomSDKSharingSourceInfo>* GetSharingSourceInfoList(unsigned int) override { return &sources; }
  ZoomSDKSharingSourceInfo begin(uint32_t user = 42, uint32_t source = 900) {
    ZoomSDKSharingSourceInfo info;
    info.userid = user; info.shareSourceID = source; info.status = Sharing_Other_Share_Begin;
    info.contentType = SHARE_TYPE_DS;
    users.items = {user}; sources.items = {info};
    return info;
  }
};
struct Renderer : IZoomSDKRendererStub {
  IZoomSDKRendererDelegate* delegate = nullptr;
  uint32_t source = 0;
  int subscriptions = 0, unsubscriptions = 0;
  SDKError result = SDKERR_SUCCESS;
  ZoomSDKResolution resolution = ZoomSDKResolution_NoUse;
  SDKError setRawDataResolution(ZoomSDKResolution value) override { resolution = value; return SDKERR_SUCCESS; }
  SDKError subscribe(uint32_t id, ZoomSDKRawDataType type) override {
    require(type == RAW_DATA_TYPE_SHARE, "Screenshot renderer must never subscribe to participant video");
    source = id; ++subscriptions; return result;
  }
  SDKError unSubscribe() override { ++unsubscriptions; return SDKERR_SUCCESS; }
};
struct ShareFrame : YUVRawDataI420Stub {
  std::vector<unsigned char> bytes = std::vector<unsigned char>(24, 128);
  int references = 1;
  unsigned int source = 900;
  unsigned long long timestamp = 12345;
  bool AddRef() override { ++references; return true; }
  int Release() override { if (--references == 0) bytes.clear(); return references; }
  char* GetYBuffer() override { return reinterpret_cast<char*>(bytes.data()); }
  char* GetUBuffer() override { return GetYBuffer() + 16; }
  char* GetVBuffer() override { return GetYBuffer() + 20; }
  unsigned int GetStreamWidth() override { return 4; }
  unsigned int GetStreamHeight() override { return 4; }
  unsigned int GetBufferLen() override { return bytes.size(); }
  unsigned int GetSourceID() override { return source; }
  unsigned long long GetTimeStamp() override { return timestamp; }
  bool IsLimitedI420() override { return true; }
};
struct Meeting : IMeetingServiceStub {
  IMeetingServiceEvent* listener = nullptr;
  Participants participants;
  Recording recording;
  Audio audio;
  Reminder reminder;
  ShareController share;
  int joins = 0, starts = 0, leaves = 0;
  uint64_t number = 0;
  std::string zak, password;
  LeaveMeetingCmd leave_command = END_MEETING;
  SDKError SetEvent(IMeetingServiceEvent* value) override { listener = value; return SDKERR_SUCCESS; }
  SDKError Join(JoinParam& params) override {
    require(params.userType == SDK_UT_WITHOUT_LOGIN, "Native join must use without-login user type");
    auto& user = params.param.withoutloginuserJoin;
    require(user.app_privilege_token == nullptr, "User ZAK must not be passed as app privilege token");
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
  IMeetingShareController* GetMeetingShareController() override { return &share; }
};
struct Backend { Auth auth; Meeting meeting; Settings settings; Raw raw; Renderer renderer; int cleanup = 0, renderers = 0; };
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
SDKError createRenderer(IZoomSDKRenderer** out, IZoomSDKRendererDelegate* delegate) {
  backend->renderer.delegate = delegate; *out = &backend->renderer; ++backend->renderers; return SDKERR_SUCCESS;
}
SDKError destroyRenderer(IZoomSDKRenderer*) {
  auto* delegate = backend->renderer.delegate; backend->renderer.delegate = nullptr;
  if (delegate) delegate->onRendererBeDestroyed();
  return SDKERR_SUCCESS;
}
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
  require(sdk.meeting.zak == "secret-zak" && sdk.meeting.number == 123456789, "Join must carry the configured user ZAK and meeting ID");
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

void sdk_permission_controls_recording() {
  for (int mode = 0; mode < 3; ++mode) {
    Backend sdk; backend = &sdk; Output output;
    if (mode != 1) sdk.meeting.participants.self.role = USERROLE_ATTENDEE;
    if (mode != 0) sdk.meeting.recording.permission = SDKERR_NO_PERMISSION;
    CaptureRuntime capture(output.root, "session");
    {
      ZoomSession session(config(), capture); admit(session, sdk);
      require(sdk.meeting.recording.starts == (mode == 0 ? 1 : 0) &&
              sdk.raw.subscriptions == (mode == 0 ? 1 : 0),
              "SDK recording approval must permit attendees and deny unauthorized hosts");
      session.stop("test_stop");
    }
    capture.close();
    require(output.events("capture.started").size() == (mode == 0 ? 1 : 0),
            "Capture success must match SDK recording permission");
  }
}

void attendee_requests_recording_once_and_waits_for_grant() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    Backend sdk; backend = &sdk; Output output;
    sdk.meeting.participants.self.role = USERROLE_ATTENDEE;
    sdk.meeting.recording.permission = SDKERR_NO_PERMISSION;
    sdk.meeting.recording.request_support = SDKERR_SUCCESS;
    if (scenario == 3) sdk.meeting.recording.request_result = SDKERR_NO_PERMISSION;
    CaptureRuntime capture(output.root, "session");
    {
      ZoomSession session(config(), capture); admit(session, sdk);
      require(sdk.meeting.recording.requests == 1 && sdk.raw.subscriptions == 0,
              "An attendee must request permission without starting unauthorized capture");
      sdk.meeting.audio.listener->onUserAudioStatusChange(nullptr, nullptr); session.tick();
      require(sdk.meeting.recording.requests == 1, "Pending or failed requests must not be repeated");
      if (scenario == 0) {
        sdk.meeting.recording.permission = SDKERR_SUCCESS;
        sdk.meeting.recording.listener->onLocalRecordingPrivilegeRequestStatus(RequestLocalRecording_Granted);
        session.tick();
        require(sdk.raw.subscriptions == 1, "An approved request must start per-user capture");
        sdk.meeting.recording.permission = SDKERR_NO_PERMISSION;
        sdk.meeting.recording.listener->onRecordPrivilegeChanged(false); session.tick();
        require(sdk.raw.unsubscriptions == 1 && sdk.meeting.recording.requests == 1,
                "Revocation must stop capture without another request");
      } else if (scenario != 3) {
        sdk.meeting.recording.listener->onLocalRecordingPrivilegeRequestStatus(
            scenario == 1 ? RequestLocalRecording_Denied : RequestLocalRecording_Timeout);
        session.tick();
        require(sdk.raw.subscriptions == 0 && sdk.meeting.recording.requests == 1,
                "Denial and timeout must not authorize capture or repeat requests");
        if (scenario == 1) {
          sdk.meeting.recording.permission = SDKERR_SUCCESS;
          sdk.meeting.audio.listener->onUserAudioStatusChange(nullptr, nullptr); session.tick();
          require(sdk.raw.subscriptions == 0, "A denied response requires an explicit later grant");
          sdk.meeting.recording.listener->onRecordPrivilegeChanged(true); session.tick();
          require(sdk.raw.subscriptions == 1, "A later explicit grant may authorize a denied attendee");
        }
      }
      session.stop("test_stop");
    }
    capture.close();
    int requests = 0;
    for (const auto& event : output.events("sdk.operation"))
      if (event["data"]["operation"] == "request_local_recording_privilege") {
        ++requests;
        require(event["data"]["result"] == (scenario == 3 ? SDKERR_NO_PERMISSION : SDKERR_SUCCESS),
                "The request operation must preserve its SDK result");
      }
    require(requests == 1, "Journal the permission request exactly once");
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

void reconnect_and_role_changes_preserve_permissions() {
  Backend sdk; backend = &sdk; Output output;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture); admit(session, sdk);
    sdk.meeting.participants.self.role = USERROLE_ATTENDEE;
    sdk.meeting.participants.listener->onHostChangeNotification(42); session.tick();
    require(sdk.raw.unsubscriptions == 0, "Role changes must preserve explicitly authorized capture");
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(false); session.tick();
    require(sdk.raw.unsubscriptions == 1, "Revoked attendee permission must stop collection");
    sdk.meeting.participants.self.role = USERROLE_COHOST;
    sdk.meeting.participants.listener->onCoHostChangeNotification(1, true); session.tick();
    require(sdk.raw.subscriptions == 1, "A role change must not override revoked recording permission");
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(true); session.tick();
    require(sdk.raw.subscriptions == 2, "Restored SDK permission must resume collection");
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
      // Linux 7.2.1 can deliver two 10ms frames with the same source timestamp.
      Packet packet; packet.timestamp = 9000 + (offset / 320) * 20;
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
  const auto packets = output.events("sdk.raw_audio.one_way");
  require(packets.size() == ((speech.size() + 159) / 160) * 2,
          "Repeated SDK timestamps must retain every frame for both speakers");
  require(output.events("audio.out_of_order").empty() && output.events("audio.gap").empty(),
          "Quantized SDK timestamps must produce continuous speaker timelines");
  const auto chunks = output.events("audio.chunk_ready");
  std::set<uint32_t> speakers;
  for (const auto& chunk : chunks) {
    speakers.insert(chunk["data"]["user_id"].get<uint32_t>());
    const auto path = chunk["data"]["wav_path"].get<std::string>();
    const auto display = chunk["data"]["user_id"] == 42 ? "Alice" : "Bob";
    require(chunk["data"]["display_name"] == display && path.find("recording-1__" + std::string(display) + "__") == 0,
            "SDK participant names must reach per-speaker WAV filenames and metadata");
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

void cloud_status_and_renames_reach_the_capture_consumer() {
  Backend sdk; backend = &sdk; Output output;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture); admit(session, sdk);
    auto* listener = sdk.meeting.recording.listener;
    listener->onCloudRecordingStatus(Recording_Start);
    listener->onCloudRecordingStatus(Recording_Start);
    listener->onCloudRecordingStatus(Recording_Pause);
    listener->onCloudRecordingStatus(Recording_Pause);
    listener->onCloudRecordingStatus(Recording_Start);
    listener->onCloudRecordingStatus(Recording_Stop);
    listener->onCloudRecordingStatus(Recording_Start);
    sdk.meeting.participants.alice.name = "홍길동";
    List<uint32_t> renamed; renamed.items = {42};
    sdk.meeting.participants.listener->onUserNamesChanged(&renamed);
    session.tick();
    require(sdk.meeting.recording.starts == 1 && sdk.raw.subscriptions == 1,
            "Cloud status notifications must not restart raw capture or control cloud recording");
    session.stop("test_stop");
  }
  capture.close();
  require(!capture.failed(), "Cloud state changes and participant renames must drain without failure");
  const auto states = output.events("cloud.recording_state");
  require(states.size() == 5 && states[0]["data"]["recording_id"] == "recording-1" &&
          states[2]["data"]["recording_id"] == "recording-1" && states[4]["data"]["recording_id"] == "recording-2",
          "Initial and repeated SDK starts must be distinguished from stop/start and pause/resume");
  require(output.events("participant.renamed").back()["data"]["display_name"] == "홍길동",
          "SDK rename callback must refresh the stored participant display name");
  int callbacks = 0;
  for (const auto& event : output.events("sdk.callback"))
    if (event["data"]["callback"] == "onCloudRecordingStatus") ++callbacks;
  require(callbacks == 7, "Duplicate cloud status callbacks must remain in the exhaustive event journal");
}

void admission_refreshes_names_received_before_user_info() {
  Backend sdk; backend = &sdk; Output output;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture);
    session.start(); sdk.auth.listener->onAuthenticationReturn(AUTHRET_SUCCESS); session.tick();
    sdk.meeting.participants.alice_available = false;
    List<uint32_t> joined; joined.items = {42};
    sdk.meeting.participants.listener->onUserJoin(&joined, nullptr);
    sdk.meeting.participants.alice_available = true;
    sdk.meeting.participants.alice.name = "수현 이";
    sdk.meeting.listener->onMeetingStatusChanged(MEETING_STATUS_INMEETING, 0); session.tick();
    SF_INFO info{};
    auto* file = sf_open(FVAD_FIXTURE, SFM_READ, &info);
    require(file != nullptr, "Speech fixture must load");
    Packet packet; packet.samples.resize(info.frames);
    sf_readf_short(file, packet.samples.data(), info.frames); sf_close(file);
    sdk.raw.delegate->onOneWayAudioRawDataReceived(&packet, 42);
    session.stop("test_stop");
  }
  capture.close();
  const auto renamed = output.events("participant.renamed");
  require(!renamed.empty() && renamed[0]["data"]["display_name"] == "수현 이",
          "Admission snapshots must replace empty early names without requiring a name-change callback");
  const auto chunks = output.events("audio.chunk_ready");
  require(!chunks.empty() && chunks[0]["data"]["wav_path"].get<std::string>().find("__수현 이__") != std::string::npos,
          "WAV filenames must use the recovered display name instead of the generic speaker label");
}

void share_samples_static_frames_at_configured_intervals() {
  Backend sdk; backend = &sdk; Output output; ShareFrame frame;
  CaptureRuntime capture(output.root, "session");
  capture.admitted(); capture.cloud_recording(0);
  {
    ShareCapture share(capture, 2);
    share.tick(0);
    require(sdk.renderers == 0, "No sharing must not create a renderer or images");
    sdk.meeting.share.begin();
    share.refresh(&sdk.meeting.share);
    require(sdk.renderer.source == 900 && sdk.renderer.resolution == ZoomSDKResolution_360P,
            "Late admission must subscribe to the actual share source ID at 360p");
    sdk.renderer.delegate->onRawDataFrameReceived(&frame);
    frame.Release();
    require(frame.references == 0, "Do not retain SDK frames; copy pixels before the callback buffer expires");
    share.tick(0); share.tick(1999); share.tick(2000);
    sdk.renderer.delegate->onRawDataStatusChanged(IZoomSDKRendererDelegate::RawData_Off);
    share.tick(4000);
    require(frame.references == 0, "Raw share off must release the cached frame and stop stale screenshots");
    share.stop(); share.tick(6000);
  }
  capture.close();
  require(!capture.failed(), "Shared-screen samples must drain without capture failure");
  const auto images = output.events("screenshot.saved");
  require(images.size() == 2 && images[0]["data"]["capture_ms"] == 0 && images[1]["data"]["capture_ms"] == 2000,
          "Save static slides every configured interval without requiring a new raw frame");
  require(images[1]["data"]["source_timestamp_ms"] == 12345 && images[1]["data"]["share_source_id"] == 900,
          "Keep source timestamps separate from sampled recording time");
}

void share_permission_reconnect_and_switching() {
  Backend sdk; backend = &sdk; Output output; ShareFrame first, second;
  sdk.meeting.share.begin();
  sdk.meeting.recording.permission = SDKERR_NO_PERMISSION;
  CaptureRuntime capture(output.root, "session");
  {
    ZoomSession session(config(), capture); admit(session, sdk);
    require(sdk.renderers == 0, "Denied recording must not subscribe to shared-screen data");
    sdk.meeting.recording.permission = SDKERR_SUCCESS;
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(true); session.tick();
    require(sdk.renderers == 1, "Recording approval must discover an already active screen share");
    sdk.renderer.delegate->onRawDataFrameReceived(&first); first.Release(); session.tick();
    auto changed = sdk.meeting.share.begin(73, 901);
    sdk.meeting.share.listener->onSharingStatus(changed); session.tick();
    require(sdk.renderer.source == 901,
            "A new sharer must release the old frame and subscribe to the new source ID");
    sdk.renderer.delegate->onRawDataFrameReceived(&second); second.Release(); session.tick();
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(false);
    require(second.references == 0, "Screenshot capture must not keep SDK callback buffers alive");
    session.tick();
    require(sdk.renderer.delegate == nullptr, "Revocation must destroy the share renderer");
    sdk.meeting.recording.listener->onRecordPrivilegeChanged(true); session.tick();
    sdk.meeting.listener->onMeetingStatusChanged(MEETING_STATUS_RECONNECTING, 0); session.tick();
    require(sdk.renderer.delegate == nullptr, "Reconnect must destroy share subscriptions");
    sdk.meeting.listener->onMeetingStatusChanged(MEETING_STATUS_INMEETING, 0); session.tick();
    changed.status = Sharing_Other_Share_End;
    sdk.meeting.share.users.items.clear(); sdk.meeting.share.sources.items.clear();
    sdk.meeting.share.listener->onSharingStatus(changed); session.tick();
    require(sdk.renderer.delegate == nullptr, "Sharing end must destroy the renderer instead of saving the last slide forever");
    session.stop("test_stop");
  }
  capture.close();
  require(!capture.failed() && output.events("screenshot.saved").size() == 2,
          "Sharing transitions must keep two sampled images without unauthorized or stale images");
}

void share_snapshots_preserve_pixels_and_every_frame_event() {
  Backend sdk; backend = &sdk; Output output; ShareFrame frame;
  CaptureRuntime capture(output.root, "session");
  capture.admitted(); capture.cloud_recording(0);
  {
    ShareCapture share(capture, 1);
    sdk.meeting.share.begin(); share.refresh(&sdk.meeting.share);
    std::fill(frame.bytes.begin(), frame.bytes.begin() + 16, 16);
    for (int i = 0; i < 60; ++i) {
      ++frame.timestamp;
      sdk.renderer.delegate->onRawDataFrameReceived(&frame);
    }
    share.tick(0);
    std::fill(frame.bytes.begin(), frame.bytes.begin() + 16, 235);
    sdk.renderer.delegate->onRawDataFrameReceived(&frame); share.tick(1000);
    std::fill(frame.bytes.begin(), frame.bytes.begin() + 16, 16);
    sdk.renderer.delegate->onRawDataFrameReceived(&frame); share.tick(2000);
    frame.Release(); share.tick(3000);
    require(frame.references == 0, "Shared snapshots must own pixels without retaining SDK buffers");
    sdk.renderer.delegate->onRawDataStatusChanged(IZoomSDKRendererDelegate::RawData_Off);
    share.tick(4000);
  }
  capture.close();
  const auto images = output.events("screenshot.saved");
  require(!capture.failed() && images.size() == 4,
          "Changed and static snapshots must preserve the configured interval without stale off-stream images");
  require(output.events("sdk.raw_share.frame").size() == 62,
          "Repeated identical pixels must still retain every raw-frame event");
  for (size_t i = 0; i < images.size(); ++i) {
    require(images[i]["data"]["source_timestamp_ms"] == frame.timestamp &&
            images[i]["data"]["capture_ms"] == i * 1000,
            "Pixel caching must preserve current source metadata and each sample's capture time");
    const auto path = output.root / images[i]["data"]["image_path"].get<std::string>();
    FILE* file = fopen(path.c_str(), "rb");
    require(file != nullptr, "Every queued snapshot must publish an image");
    jpeg_decompress_struct decoder{}; jpeg_error_mgr error{};
    decoder.err = jpeg_std_error(&error);
    jpeg_create_decompress(&decoder); jpeg_stdio_src(&decoder, file);
    jpeg_read_header(&decoder, TRUE); decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    require(decoder.output_width == 640 && decoder.output_height == 360 && decoder.output_components == 3,
            "Cached snapshots must remain 640x360 RGB JPEGs");
    std::vector<unsigned char> row(640 * 3);
    while (decoder.output_scanline <= 180) {
      JSAMPROW pixels = row.data(); jpeg_read_scanlines(&decoder, &pixels, 1);
    }
    require(i == 1 ? row[320 * 3] > 245 : row[320 * 3] < 10,
            "Queued snapshots must stay immutable when pixels change with the same SDK timestamp");
    jpeg_abort_decompress(&decoder); jpeg_destroy_decompress(&decoder); fclose(file);
  }
}

int main() {
  try {
    rejected_auth_never_joins(); sdk_permission_controls_recording();
    attendee_requests_recording_once_and_waits_for_grant();
    revocation_stops_and_grant_resumes(); reconnect_and_role_changes_preserve_permissions(); delayed_audio_and_subscription_failure();
    combined_recording_notices();
    real_pcm_fixture_through_sdk_callbacks();
    cloud_status_and_renames_reach_the_capture_consumer();
    admission_refreshes_names_received_before_user_info();
    share_samples_static_frames_at_configured_intervals();
    share_permission_reconnect_and_switching();
    share_snapshots_preserve_pixels_and_every_frame_event();
    std::cout << "20 SDK adapter scenarios passed (test doubles, no live meeting)\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
