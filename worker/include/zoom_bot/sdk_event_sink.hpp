#pragma once

#include "zoom_bot/sdk_headers.hpp"
#include <nlohmann/json.hpp>
#include <type_traits>

namespace zoom_bot {
using namespace ZOOMSDK;
class SdkEventSink {
 public:
  virtual ~SdkEventSink() = default;
  virtual void record(const char* interface, const char* callback, nlohmann::json data) = 0;
  virtual void failed() noexcept = 0;
};

inline nlohmann::json sdk_value(const char* value) { return value ? nlohmann::json(value) : nlohmann::json(nullptr); }
inline nlohmann::json sdk_value(char* value) { return sdk_value(static_cast<const char*>(value)); }
template <typename T> nlohmann::json sdk_value(T value) {
  if constexpr (std::is_enum_v<T>) return static_cast<int>(value);
  else if constexpr (std::is_arithmetic_v<T>) return value;
  else return {{"opaque", true}};
}
// Action handlers are SDK-owned and expire after callbacks. Record presence, never pointer addresses.
template <typename T> nlohmann::json sdk_value(T* value) {
  return value ? nlohmann::json{{"present", true}, {"opaque", true}} : nlohmann::json(nullptr);
}
inline nlohmann::json sdk_value(IUserAudioStatus* value) {
  if (!value) return nullptr;
  return {{"user_id", value->GetUserId()}, {"status", sdk_value(value->GetStatus())},
          {"audio_type", sdk_value(value->GetAudioType())}};
}
inline nlohmann::json sdk_value(IMicInfo* value) {
  if (!value) return nullptr;
  return {{"device_id", sdk_value(value->GetDeviceId())}, {"device_name", sdk_value(value->GetDeviceName())},
          {"selected", value->IsSelectedDevice()}};
}
inline nlohmann::json sdk_value(ISpeakerInfo* value) {
  if (!value) return nullptr;
  return {{"device_id", sdk_value(value->GetDeviceId())}, {"device_name", sdk_value(value->GetDeviceName())},
          {"selected", value->IsSelectedDevice()}};
}
inline nlohmann::json sdk_value(IAccountInfo* value) {
  if (!value) return nullptr;
  return {{"display_name", sdk_value(value->GetDisplayName())}, {"login_type", sdk_value(value->GetLoginType())}};
}
inline nlohmann::json sdk_value(IProxySettingHandler* value) {
  if (!value) return nullptr;
  return {{"host", sdk_value(value->GetProxyHost())}, {"port", value->GetProxyPort()},
          {"description", sdk_value(value->GetProxyDescription())}};
}
inline nlohmann::json sdk_value(ISSLCertVerificationHandler* value) {
  if (!value) return nullptr;
  return {{"issued_to", sdk_value(value->GetCertIssuedTo())}, {"issued_by", sdk_value(value->GetCertIssuedBy())},
          {"serial_number", sdk_value(value->GetCertSerialNum())}, {"fingerprint", sdk_value(value->GetCertFingerprint())}};
}
inline nlohmann::json sdk_value(const MeetingParameter* value) {
  if (!value) return nullptr;
  return {{"meeting_type", sdk_value(value->meeting_type)}, {"is_view_only", value->is_view_only},
          {"auto_recording_local", value->is_auto_recording_local},
          {"auto_recording_cloud", value->is_auto_recording_cloud}, {"meeting_number", value->meeting_number},
          {"topic", sdk_value(value->meeting_topic)}, {"host", sdk_value(value->meeting_host)}};
}
template <typename T> nlohmann::json sdk_value(IList<T>* list);
inline nlohmann::json sdk_value(IMeetingReminderContent* value) {
  if (!value) return nullptr;
  return {{"type", sdk_value(value->GetType())}, {"title", sdk_value(value->GetTitle())},
          {"content", sdk_value(value->GetContent())}, {"blocking", value->IsBlocking()},
          {"action_type", sdk_value(value->GetActionType())}, {"types", sdk_value(value->GetMultiReminderTypes())}};
}
inline nlohmann::json sdk_value(CustomWaitingRoomData& value) {
  return {{"title", sdk_value(value.title)}, {"description", sdk_value(value.description)},
          {"logo_path", sdk_value(value.logo_path)}, {"video_path", sdk_value(value.video_path)},
          {"image_path", sdk_value(value.image_path)}, {"layout", sdk_value(value.type)},
          {"status", sdk_value(value.status)}};
}
inline nlohmann::json sdk_value(IChatMsgInfo* value) {
  if (!value) return nullptr;
  return {{"message_id", sdk_value(value->GetMessageID())}, {"sender_id", value->GetSenderUserId()},
          {"sender_name", sdk_value(value->GetSenderDisplayName())}, {"receiver_id", value->GetReceiverUserId()},
          {"receiver_name", sdk_value(value->GetReceiverDisplayName())}, {"content", sdk_value(value->GetContent())},
          {"timestamp", value->GetTimeStamp()}, {"message_type", sdk_value(value->GetChatMessageType())},
          {"thread_id", sdk_value(value->GetThreadID())}, {"is_comment", value->IsComment()}, {"is_thread", value->IsThread()}};
}
inline nlohmann::json sdk_value(ChatStatus* value) {
  if (!value) return nullptr;
  nlohmann::json result = {{"is_chat_off", value->is_chat_off}, {"is_webinar_attendee", value->is_webinar_attendee},
                           {"is_webinar_meeting", value->is_webinar_meeting}};
  if (!value->is_webinar_meeting) {
    const auto& status = value->ut.normal_meeting_status;
    result["permissions"] = {{"can_chat", status.can_chat}, {"to_all", status.can_chat_to_all},
                              {"to_individual", status.can_chat_to_individual}, {"host_only", status.is_only_can_chat_to_host}};
  } else if (value->is_webinar_attendee) {
    const auto& status = value->ut.webinar_attendee_status;
    result["permissions"] = {{"can_chat", status.can_chat}, {"to_all", status.can_chat_to_all_panellist_and_attendee},
                              {"to_panelists", status.can_chat_to_all_panellist}};
  } else {
    const auto& status = value->ut.webinar_other_status;
    result["permissions"] = {{"to_all", status.can_chat_to_all_panellist_and_attendee},
                              {"to_panelists", status.can_chat_to_all_panellist}, {"to_individual", status.can_chat_to_individual}};
  }
  return result;
}
inline nlohmann::json sdk_value(ZoomSDKSharingSourceInfo value) {
  return {{"user_id", value.userid}, {"share_source_id", value.shareSourceID}, {"status", sdk_value(value.status)},
          {"first_view", value.isShowingInFirstView}, {"second_view", value.isShowingInSecondView},
          {"remote_control", value.isCanBeRemoteControl}, {"optimizing_video", value.bEnableOptimizingVideoSharing},
          {"content_type", sdk_value(value.contentType)}, {"monitor_id", sdk_value(value.monitorID)}};
}
inline nlohmann::json sdk_value(SDKFileTransferInfo* value) {
  if (!value) return nullptr;
  return {{"message_id", sdk_value(value->messageID)}, {"status", sdk_value(value->trans_status)},
          {"timestamp", value->time_stamp}, {"send_to_all", value->is_send_to_all},
          {"file_size", value->file_size}, {"file_name", sdk_value(value->file_name)},
          {"complete_percentage", value->complete_percentage}, {"complete_size", value->complete_size},
          {"bytes_per_second", value->bit_per_second}};
}
inline nlohmann::json sdk_value(ISDKFileSender* value) {
  if (!value) return nullptr;
  return {{"receiver_id", value->GetReceiver()}, {"transfer", sdk_value(value->GetTransferInfo())}};
}
inline nlohmann::json sdk_value(ISDKFileReceiver* value) {
  if (!value) return nullptr;
  return {{"sender_id", value->GetSender()}, {"transfer", sdk_value(value->GetTransferInfo())}};
}
template <typename T> nlohmann::json sdk_value(IList<T>* list) {
  if (!list) return nullptr;
  auto values = nlohmann::json::array();
  for (int i = 0; i < list->GetCount(); ++i) values.push_back(sdk_value(list->GetItem(i)));
  return values;
}
}  // namespace zoom_bot
