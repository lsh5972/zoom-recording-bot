#!/usr/bin/env python3
"""Fetch public Doxygen header definitions for compile checks, not SDK binaries."""

import hashlib
import json
from html.parser import HTMLParser
from pathlib import Path
import re
import urllib.request

ROOT = "https://marketplacefront.zoom.us/sdk/meeting/linux/"
DESTINATION = Path(__file__).resolve().parents[1] / "vendor/zoom-api/h"
SEEDS = ["zoom_sdk.h", "auth_service_interface.h", "meeting_service_interface.h",
         "meeting_service_components/meeting_audio_interface.h",
         "meeting_service_components/meeting_participants_ctrl_interface.h",
         "meeting_service_components/meeting_recording_interface.h",
         "meeting_service_components/meeting_video_interface.h",
         "meeting_service_components/meeting_sharing_interface.h",
         "meeting_service_components/meeting_chat_interface.h",
         "meeting_service_components/meeting_waiting_room_interface.h",
         "meeting_service_components/meeting_reminder_ctrl_interface.h",
         "meeting_service_components/meeting_breakout_rooms_interface_v2.h",
         "meeting_service_components/meeting_configuration_interface.h",
         "meeting_service_components/meeting_ai_companion_interface.h",
         "meeting_service_components/meeting_inmeeting_encryption_interface.h",
         "meeting_service_components/meeting_raw_archiving_interface.h",
         "meeting_service_components/meeting_webinar_interface.h",
         "setting_service_interface.h", "network_connection_handler_interface.h", "zoom_sdk_raw_data_def.h",
         "rawdata/rawdata_audio_helper_interface.h", "rawdata/zoom_rawdata_api.h"]


class SourceParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.lines = []
        self.current = None
        self.span_depth = 0
        self.number_depth = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "div" and attrs.get("class") == "line":
            self.current = []
        if self.current is not None and tag == "span":
            self.span_depth += 1
            if attrs.get("class") == "lineno":
                self.number_depth = self.span_depth

    def handle_endtag(self, tag):
        if self.current is not None and tag == "span":
            if self.number_depth == self.span_depth:
                self.number_depth = None
            self.span_depth -= 1
        if self.current is not None and tag == "div":
            self.lines.append("".join(self.current).replace("\xa0", " "))
            self.current = None

    def handle_data(self, data):
        if self.current is not None and self.number_depth is None:
            self.current.append(data)


def main():
    pending = list(SEEDS)
    manifest = {}
    while pending:
        relative = pending.pop(0)
        if relative in manifest:
            continue
        basename = Path(relative).name
        page = basename.replace("_", "__").replace(".h", "_8h_source.html")
        url = ROOT + page
        path = DESTINATION / relative
        if path.exists():
            source = path.read_text()
        else:
            parser = SourceParser()
            with urllib.request.urlopen(url, timeout=30) as response:
                parser.feed(response.read().decode("utf-8"))
            if not parser.lines:
                raise RuntimeError(f"No public header source at {url}")
            source = "\n".join(parser.lines) + "\n"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        manifest[relative] = {"url": url, "sha256": hashlib.sha256(source.encode()).hexdigest()}
        for include in re.findall(r'^\s*#include\s+"([^"]+)"', source, re.MULTILINE):
            if include not in manifest:
                pending.append(include)
        print(f"Fetched public API definition: {relative}")
    (DESTINATION.parent / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Downloaded {len(manifest)} public API headers. SDK binary/runtime not included.")


if __name__ == "__main__":
    main()
