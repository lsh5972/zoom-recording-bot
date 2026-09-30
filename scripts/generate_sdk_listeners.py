#!/usr/bin/env python3
"""Generate exhaustive callback forwarding from the selected Linux SDK headers."""

import argparse
from pathlib import Path
import re
import subprocess

INTERFACES = ["IAuthServiceEvent", "IMeetingServiceEvent", "IMeetingParticipantsCtrlEvent",
              "IMeetingRecordingCtrlEvent", "IMeetingAudioCtrlEvent", "IMeetingVideoCtrlEvent",
              "IMeetingShareCtrlEvent", "IMeetingChatCtrlEvent", "IMeetingWaitingRoomEvent",
              "IMeetingReminderEvent", "IMeetingConfigurationEvent", "IMeetingBOControllerEvent",
              "IBOCreatorEvent", "IBOAdminEvent", "IBOAttendeeEvent", "IBODataEvent",
              "IMeetingWebinarCtrlEvent", "IMeetingEncryptionControllerEvent",
              "IMeetingAICompanionCtrlEvent", "IMeetingAICompanionQueryHelperEvent",
              "IMeetingAICompanionSmartSummaryHelperEvent", "IAudioSettingContextEvent", "INetworkConnectionHandler"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--headers", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--stubs", help="Generate test doubles, never linked into the real worker")
    args = parser.parse_args()
    include = Path(__file__).resolve().parents[1] / "worker/include"
    source = subprocess.run([args.compiler, "-E", "-P", "-x", "c++", "-std=c++17", "-D__linux__",
                             "-I", args.headers, "-I", str(include), "-"],
                            input='#include "zoom_bot/sdk_headers.hpp"\n', text=True,
                            capture_output=True, check=True).stdout
    classes = {name: (parent, body) for name, parent, body in
               re.findall(r"class\s+(\w+)\s*(?::\s*public\s+(\w+))?\s*\{(.*?)\n\};", source, re.S)}

    def methods(name):
        parent, body = classes[name]
        inherited = methods(parent) if parent else []
        own = re.findall(r"virtual\s+void\s+(\w+)\s*\((.*?)\)\s*=\s*0\s*;", body, re.S)
        return inherited + own

    output = ['// Generated from SDK interfaces. Do not edit; run generate_sdk_listeners.py.',
              '#pragma once', '#include "zoom_bot/sdk_event_sink.hpp"', '#include <functional>',
              'namespace zoom_bot {', 'using namespace ZOOMSDK;']
    total = 0
    for interface in INTERFACES:
        callbacks = methods(interface)
        if not callbacks:
            raise RuntimeError(f"No callbacks extracted from {interface}")
        output += [f'class {interface}Listener final : public {interface} {{', ' public:',
                   f'  explicit {interface}Listener(SdkEventSink& sink) : sink_(sink) {{}}']
        for method, declaration in callbacks:
            declaration = re.sub(r"\s+", " ", declaration).strip()
            params = []
            for param in re.split(r",(?![^<]*>)", declaration) if declaration else []:
                param = param.split("=")[0].strip()
                match = re.search(r"(\w+)\s*$", param)
                if not match:
                    raise RuntimeError(f"Unsupported parameter: {param}")
                params.append((param, match.group(1)))
            signature = ', '.join(param for param, _ in params)
            names = ', '.join(name for _, name in params)
            fields = ', '.join('{"' + name + '", sdk_value(' + name + ')}' for _, name in params)
            output += [f'  std::function<void({signature})> after_{method};',
                       f'  void {method}({signature}) override {{', '    try {',
                       f'      sink_.record("{interface}", "{method}", nlohmann::json::object({{{fields}}}));',
                       f'      if (after_{method}) after_{method}({names});',
                       '    } catch (...) { sink_.failed(); }', '  }']
            total += 1
        inventory = ', '.join('"' + method + '"' for method, _ in callbacks)
        output += [f'  static nlohmann::json callbacks() {{ return {{{inventory}}}; }}',
                   ' private:', '  SdkEventSink& sink_;', '};']
    inventory = ', '.join('{"' + interface + '", ' + interface + 'Listener::callbacks()}' for interface in INTERFACES)
    output += ['inline nlohmann::json sdk_callback_inventory() { return {' + inventory + '}; }',
               '}  // namespace zoom_bot', '']
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(output))
    print(f"Generated {total} callbacks in {len(INTERFACES)} Linux interfaces")
    if args.stubs:
        interfaces = ["IAuthService", "IMeetingService", "ISettingService", "IAudioSettingContext",
                      "IMeetingParticipantsController", "IMeetingRecordingController", "IMeetingAudioController",
                      "IUserInfo", "AudioRawData", "IZoomSDKAudioRawDataHelper"]
        stubs = ['// Generated test doubles. Not a Zoom SDK runtime.', '#pragma once',
                 '#include "zoom_bot/sdk_headers.hpp"', 'using namespace ZOOMSDK;']
        for interface in interfaces:
            _, body = classes[interface]
            stubs += [f'class {interface}Stub : public {interface} {{', ' public:']
            virtuals = re.findall(r"virtual\s+([^;{}]+?)\s+(\w+)\s*\((.*?)\)\s*(const)?\s*=\s*0\s*;", body, re.S)
            for return_type, method, declaration, qualifier in virtuals:
                return_type = re.sub(r"\s+", " ", return_type).strip()
                declaration = re.sub(r"\s+", " ", declaration).strip()
                declaration = re.sub(r"\s*=\s*[^,]+(?=,|$)", "", declaration)
                result = '' if return_type == 'void' else 'return SDKERR_NO_IMPL;' if return_type == 'SDKError' else 'return {};'
                stubs += [f'  {return_type} {method}({declaration}) {qualifier} override {{ {result} }}']
            stubs += ['};']
        Path(args.stubs).write_text('\n'.join(stubs) + '\n')


if __name__ == "__main__":
    main()
