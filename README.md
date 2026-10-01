# Zoom bot

우리 Zoom 계정이 주최하는 회의에 전용 봇 사용자 ZAK로 들어가는 봇.
Ruby는 인증·실행을 담당하고, 공식 Linux Meeting SDK를 사용하는 C++ worker가
실시간 음성·이벤트를 처리하는 구조다. RTMS, PostgreSQL, Rails는 사용하지 않는다.

## 현재 구현 상태

구현: S2S OAuth, 회의의 실제 호스트 조회, 설정한 봇 사용자의 ZAK 발급 요청,
SDK JWT 서명, 비공개 세션 파일 생성, Docker 실행·정지 명령.
SDK와 독립된 C++ 음성 엔진은 사용자별 PCM → WebRTC VAD → 발화별 WAV,
speech_on/off·청크 이벤트 기록까지 처리한다. Ruby 전송기는 완성된 WAV를 건별 POST하고
성공한 위치를 파일에 저장한다.
HTTP 오류에서 응답 원문을 출력하지 않고, 401은 토큰 갱신 후 한 번만 재시도한다.

SDK 연결 코드와 Dockerfile도 구현했다. SDK 인증 → 봇 사용자 `userZAK` 입장 → 실제 역할·
raw recording 권한 확인 → 사용자별 PCM 콜백을 음성 엔진에 연결한다.
공개 공식 API 헤더 기준으로 macOS와 Ubuntu 22.04 ARM64 컨테이너에서 컴파일·테스트했다.
SDK 테스트는 서비스 응답을 대체하며 실제 음성 fixture를 콜백부터 WAV까지 처리한다.
실제 Linux-All SDK 7.2.1 (5860)을 Linux ARM64 Docker에서 링크·실행했다.
실제 S2S 인증·회의 호스트 조회·봇 사용자 ZAK 발급·SDK 인증·일반 참가자 입장·VoIP 연결을 확인했다.
클라우드 녹화를 유지한 회의에서 SDK 녹화 권한 요청 → 승인 응답 → raw 녹음 시작·구독까지 성공했다.
실제 화자 PCM으로 32kHz·mono·16bit WAV 두 개와 speech_on/off·청크 이벤트를 생성하고
독립 디코더로 프레임 수·음성 샘플을 확인했다. 동시 다화자의 실제 회의 검증은 아직 하지 않았다.
SDK가 연속 10ms PCM에 같은 타임스탬프를 전달하는 경우도 모든 샘플을 보존하도록 보정한다.
이전 실제 worker 종료에서 SDK 정리 중 종료 코드 139가 발생했다. 종료 안정성은 아직 검증되지 않았다.

## 실행 환경과 설정

개발 검증 환경: Ruby 3.2.5. 실행 코드는 표준 라이브러리만 사용한다.
테스트에는 Minitest가 필요하다. `.ruby-version`을 인식하는 Ruby 환경에서 실행한다.

서로 다른 두 앱의 자격증명이 필요하다.

| 앱 | 환경변수 | 역할 |
| --- | --- | --- |
| Meeting SDK를 활성화한 General App | `ZOOM_SDK_CLIENT_ID`, `ZOOM_SDK_CLIENT_SECRET` | SDK JWT 서명 |
| 호스트·봇 사용자와 같은 계정의 Server-to-Server OAuth App | `ZOOM_S2S_ACCOUNT_ID`, `ZOOM_S2S_CLIENT_ID`, `ZOOM_S2S_CLIENT_SECRET` | 회의 조회·봇 사용자 ZAK 발급 |

S2S 앱에 필요한 범위:

- `meeting:read:meeting:admin`: `GET /v2/meetings/{meetingId}`
- `user:read:token:admin`: `GET /v2/users/{botUserEmail}/token?type=zak`

SDK와 S2S의 Client ID/Secret은 서로 바꿔 쓰지 않는다.
ZAK 발급은 S2S OAuth와 Zoom REST API만 사용한다. SDK 파일·SDK 인증은 필요 없다.
`prepare`는 ZAK 발급 뒤 SDK JWT도 준비하므로 설정에 두 앱의 자격증명을 모두 요구한다.
ZAK는 사용자 인증 정보이며, 녹음 가능 여부는 입장 후 별도로 확인해야 한다.

`ZOOM_BOT_USER_EMAIL`에는 같은 Zoom 계정에 속한 전용 봇 사용자 이메일을 입력한다.
호스트 ZAK를 대신 사용하거나 호스트·공동 호스트 역할을 요구하지 않는다.
SDK JWT는 앱 인증, 봇 사용자 ZAK는 그 사용자의 Zoom identity로 입장하기 위한 인증이다.
Linux SDK의 `SDK_UT_WITHOUT_LOGIN` + `userZAK` 입장과 SDK의 상시 SSO 로그인 세션은 별개다.
[공식 인증 문서](https://developers.zoom.us/docs/meeting-sdk/auth/)

호스트에게 적용되는 `Record to computer files` 설정에서 `Internal meeting participants`와
`Auto approve their permission requests`를 켜면 내부 참가자의 녹화 요청을 자동 승인할 수 있다.
SDK는 권한이 없는 일반 참가자일 때 지원 여부를 확인하고 회의당 한 번만 요청한다.
승인 응답·`CanStartRawRecording` 성공을 확인한 뒤 캡처하며, 거절·타임아웃·권한 철회를 우회하지 않는다.
Licensed 사용자라는 사실만으로 녹화 권한이 자동 부여되었다고 가정하지 않는다.
현재 실제 승인에 적용된 세부 정책이 Internal 규칙인지 다른 규칙인지는 확인되지 않았다.
기존 S2S의 두 범위만 사용하며 계정 설정이나 클라우드 녹화를 변경하지 않는다.
[공식 컴퓨터 녹화 설정](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0063640)

```sh
cp .env.example .env
chmod 600 .env
# 로컬 편집기로 .env의 빈 값을 입력한다.
set -a
. ./.env
set +a
ruby bin/zoom-bot check
```

`check`는 필수 값의 존재만 확인한다. 실제 인증 성공을 뜻하지 않는다.

```sh
ruby bin/zoom-bot prepare 12345678901
```

`prepare`는 Zoom API로 회의와 호스트를 조회하고 설정한 봇 사용자의 새 ZAK·JWT를 준비한다.
회의에 입장하지 않으며, 반환된 토큰을 출력하지 않는다.
`runs/<session_id>/join.json`에 필요한 단기 인증 정보를 저장한다.
디렉터리는 `0700`, 파일은 `0600`으로 생성한다. `.env`, `runs/`, `vendor/`는 Git에서 제외한다.
현재 단기 토큰 파일의 자동 삭제는 없으므로 보존 정책은 후속 구현 대상이다.

## 공식 SDK 배포

공식 [Linux SDK 다운로드 안내](https://developers.zoom.us/docs/meeting-sdk/linux/get-started/download/)에 따르면
Zoom Marketplace 로그인과 등록된 앱이 하나 이상 있어야 SDK 바이너리를 다운로드할 수 있다.
General App → Features → Embed → Meeting SDK → Linux에서 받는다.
공개 [Zoom 샘플](https://github.com/zoom/meetingsdk-linux-raw-recording-sample)은 SDK 바이너리와 별도다.

SDK 확보·빌드 작업은 구현 과정에 포함된다. 계정 접근이나 약관 동의가 필요한 경우에는
사용자가 승인한 범위 안에서 진행한다. 바이너리를 저장소에 커밋하지 않는다.
현재 배포본은 `zoom-meeting-sdk-linux-7.2.1.5860.tar.xz`이고,
SHA-256은 `sdk.sha256`에 고정했다. Linux-All 아카이브에는 `arm64/`와 `x86_64/`가 함께 있다.
현재 `vendor/zoom-sdk/`는 ARM64 7.2.1이며, x86_64 원본은
`vendor/zoom-sdk-linux-7.2.1.5860/x86_64/`에 있다.
SDK를 교체할 때는 이전 디렉터리와 섞지 않고 대상 아키텍처의 배포 디렉터리 전체를 사용한다.
공식 문서상 7.0부터 ARM64를 지원한다. 로컬 Docker는 Linux ARM64이므로
ARM64 SDK와 같은 아키텍처의 Ubuntu 이미지를 사용하고, x86 에뮬레이션을 전제로 삼지 않는다.

SDK를 받을 수 없어도 공식 공개 [API 정의](https://marketplacefront.zoom.us/sdk/meeting/linux/files.html)로
입장·콜백 코드를 컴파일할 수 있다. 로그인 없이 공개 헤더 정의를 가져오는 스크립트를 제공한다.
아래 이미지는 검증용이며, SDK 바이너리를 포함하거나 실제 Zoom에 입장하지 않는다.

```sh
python3 scripts/fetch_zoom_api_headers.py
docker build --target api-check -t zoom-bot-api-check:local .
```

실제 SDK 배포본의 `h/`, `libmeetingsdk.so`, `qt_libs/`와 동봉 리소스를
`vendor/zoom-sdk/`에 배치한 뒤 운영 worker 이미지를 빌드한다.

```sh
# Apple Silicon의 네이티브 Linux ARM64 worker:
docker build --platform linux/arm64 --target worker -t zoom-bot-worker:local .
```

Docker는 자격증명·회의 출력 폴더를 빌드 context에서 제외한다. 한 회의의 worker에는
가상 스피커·무음 마이크를 사용하는 PulseAudio를 함께 시작하며, 추가 권한 컨테이너는 사용하지 않는다.

공식 Linux 소개에는 Meeting SDK의 봇·AI 노트테이커 미지원 정책 안내가 있다.
원시 녹음 샘플이 존재하는 것과 현재 서비스 용도의 지원 여부는 별개이므로
프로덕션 사용이 승인되었다고 간주하지 않는다.
[정책 안내](https://developers.zoom.us/docs/meeting-sdk/linux/)

## Zoom worker 실행

`ZOOM_BOT_IMAGE`는 로컬에 빌드된 worker 이미지 이름이다.
`run`은 이미지 존재 여부부터 확인한 뒤 새 세션을 준비한다.
위 빌드 명령을 사용했다면 `ZOOM_BOT_IMAGE='zoom-bot-worker:local'`로 설정한다.

```sh
ruby bin/zoom-bot run 12345678901
ruby bin/zoom-bot stop SESSION_ID
```

한 번의 `run`으로 컨테이너 하나를 실행한다. 동일 회의의 중복 실행 방지는 아직 없다.
컨테이너 생성 성공과 Zoom 입장 성공은 구분한다.

- 입력: 읽기 전용 `/run/zoom-bot/join.json`.
- 출력: `/data` → 호스트의 `runs/<session_id>/output/`.
- 이미지 entrypoint 인자: `--config /run/zoom-bot/join.json --output /data`.
- S2S/SDK 원본 Secret은 컨테이너에 전달하지 않는다.
- 이미지의 실행 사용자는 마운트된 `0600` 입력과 `0700` 출력에 접근할 수 있어야 한다.
- `stop`은 SIGTERM 후 최대 30초를 기다린다. worker는 그 안에 청크·이벤트를 마감해야 한다.
- 컨테이너와 출력은 자동 삭제하지 않는다. 업로드 재시도는 캡처 종료 후에도 가능하도록 별도 구현한다.

worker의 처리 순서:

1. SDK JWT 인증 → 봇 사용자 `userZAK`로 입장 → VoIP 연결.
2. `CanStartRawRecording` 확인 → 필요하면 녹화 권한 요청·승인 대기 → `StartRawRecording` → 사용자별 PCM 구독.
3. SDK 콜백에서 버퍼를 복사·큐잉하고, 별도 처리 루프에서 저장·VAD 수행.
4. 권한 철회·퇴장·종료 시 수집 중단, 진행 중 청크와 이벤트 마감.

입장·녹음 권한 대기 시간은 120초다. 인증 실패·입장 실패·큐 넘침·저장 실패는
오류 이벤트와 종료 코드로 구분한다. VoIP 연결 대기는 audio 상태 콜백과 주기 확인으로 재시도한다.
마이크는 입장 설정으로 mute하고, unmute 요청은 수락하지 않는다.
SIGTERM은 SDK main thread에서 raw 구독·녹음을 종료하고 `LEAVE_MEETING`으로 봇만 퇴장시킨다.
SDK 정리 후 큐를 비우고 WAV·이벤트를 마감한다. 기존 다른 회의를 종료하라는 요청은 취소한다.

호스트가 먼저 들어온 경우, 봇이 먼저 들어온 경우, 재접속한 경우를 각각 실제 회의로 검증해야 한다.
봇 사용자 ZAK만으로 녹화 권한이 보장된다고 가정하지 않는다.

## 음성·이벤트 처리

책임 분리: `ZoomSession`은 SDK 수명주기, `SpeechDetector`는 VAD,
`AudioSegmenter`는 청크 경계, `WavWriter`는 WAV 저장, `EventJournal`은 이벤트 기록,
Ruby `ChunkDelivery`는 건별 전달·재시도를 담당한다.

- 사용자별 PCM에 VAD를 적용한다. 한 기기를 공유하는 여러 사람의 목소리 분리는 범위 밖이다.
- 기본은 발화 단위다. VAD가 비음성으로 판정한 상태가 400ms 이어지면 마감한다.
- 발화가 30초 이어질 때만 중간 분할한다. pre-roll 200ms와 강제 분할 overlap 200ms를 사용한다.
  30초는 발화 시작부터 센다. pre-roll·overlap·무음 꼬리를 포함한 WAV 길이는 30초를 넘을 수 있다.
- 완성된 독립 WAV마다 `audio.chunk_ready`를 즉시 기록한다. 회의 종료까지 기다리지 않는다.
- 청크마다 `chunk_id`, `participant_session_id`, `utterance_id`, 순번,
  공통 시간축의 `start_ms`/`end_ms`, `overlap_ms`, `cut_reason`을 포함한다.
- `speech_on/off`는 VAD 상태 전이에서 발생한다. 강제 청크 분할이나 mute 상태와 혼동하지 않는다.
- SDK의 active-audio 콜백은 `sdk.callback`에 별도로 보존한다.
- 재입장은 새 participant session으로 구분한다. 표시 이름으로 합치지 않는다.
- 이벤트는 `events.jsonl`에 순번·수집 시간·기록 시각과 함께 저장하고 매번 동기화한다.
  SDK 이벤트 수집 범위는 아래 설명을 따른다.
- 외부 전사 구현은 범위 밖이다. 수신 측은 `chunk_id` 중복 처리와 overlap 구간을 고려해야 한다.

회의 출력 폴더 하나에 모든 화자의 WAV와 이벤트를 함께 저장한다. 화자별 하위 폴더는 만들지 않는다.

```text
runs/<session_id>/output/
  user-42-join-1__00:01:12-00:01:38__chunk-3.wav
  user-73-join-1__00:01:15-00:01:22__chunk-2.wav
  events.jsonl
  delivery.json
```

시간은 봇의 첫 회의 입장을 `00:00:00`으로 놓은 공통 상대 시간이다. SDK의 밀리초 PCM timestamp에
하나의 공통 offset을 적용한 뒤 화자별 샘플 수로 동일하거나 겹치는 SDK callback timestamp를 보정한다.
SDK timestamp 자체가 과거로 이동하는 경우는 오류로 처리하고, 실제 앞으로의 시간 간격은 gap 이벤트로 기록한다.
봇 입장 전 실제 회의 시작과의 차이는 현재 얻지 않는다. **클라우드 녹화본별 재생 시간 기준은 아직 미구현이다.**
목표는 각 클라우드 녹화본의 시작을 `00:00:00`으로 놓는 것이다. 예를 들어 같은 회의에서 1시간짜리
녹화본 두 개가 만들어지면, 두 번째 녹화본의 10초 위치는 `01:00:10`이 아닌 `00:00:10`이어야 한다.
첫 녹화본은 자동 녹화로 실제 회의 시작과 일치한다는 운영 조건을 사용한다. 후속 녹화본은 각 녹화본의
`recording_start`와 녹화 상태 변화로 구분하고, WAV·전사 메타데이터에 녹화본 식별자를 함께 담아야 한다.
파일들은 계속 하나의 회의 폴더에 저장한다. 현재 worker는 클라우드 상태 콜백을 기록하지만
그에 따라 WAV 시간축을 바꾸거나 녹화본 식별자를 붙이지 않는다.
전사 시각은 WAV 청크의 `start_ms`에 ASR의 WAV 내부 시각을 더해 계산한다.
`speech_start_ms`는 VAD가 감지한 발화 시작이고, WAV의 `start_ms`는 pre-roll을 포함하므로 서로 다를 수 있다.
일반 회의 조회의 `start_time`을 실제 시작으로 간주하지 않는다. 현재 S2S로 일반 회의 조회는 성공하지만
진행 중 회의의 실제 시작을 제공하는 Dashboard 조회는 권한 오류 4711을 반환했다.
[Zoom 공식 실제 시작 시각 안내](https://devforum.zoom.us/t/is-there-any-way-to-distinguish-meeting-has-limitation-or-not/44599/4)
현재 테스트 회의의 녹화 파일 조회는 `404 / 3301`을 반환해 녹화본 메타데이터를 아직 얻지 못했다.
[공식 녹화 조회·recording_start 정의](https://developers.zoom.us/docs/api/meetings/)
파일명의 초 단위 표시는 소수부를 버리며, JSON의 밀리초 값이 정확한 기준이다.
같은 초에 여러 청크가 생겨도 화자 세션과 순번으로 충돌하지 않는다.

## 음성 엔진 빌드·재생 검증

C++17, CMake 3.22 이상, pkg-config, libsndfile 개발 패키지가 필요하다.
CMake는 고정 버전·SHA-256으로 libfvad(WebRTC VAD)와 nlohmann/json을 받는다.
Zoom SDK 바이너리는 이 음성 엔진의 빌드에 필요하지 않다.

```sh
cmake -S worker -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure

# 입력 WAV를 공통 타임라인에서 각각 다른 Zoom 사용자로 재생한다.
build/zoom-bot-replay replay-demo /tmp/zoom-replay 42 speaker-a.wav 73 speaker-b.wav
# 실제 재생 속도로 청크가 생성되는 시점을 확인할 때:
build/zoom-bot-replay --realtime replay-live /tmp/zoom-replay-live 42 speaker-a.wav
```

Replay는 Zoom 입장이 아닌 테스트 도구다. 이미 이벤트 파일이 있는 출력 폴더는 재사용하지 않는다.
SDK 어댑터는 사용자별 PCM을 복사해 32MiB·20,000개 상한 큐에 넣는다. 단일 consumer가
공통 타임라인 변환·저장·VAD를 담당한다. 큐가 넘치면 녹음을 실패 처리하고 조용히 버리지 않는다.
권한 중단은 진행 중 발화를 마감하지만 기존 화자 세션·청크 순번은 유지한다.

## SDK 이벤트 수집 범위

`generate_sdk_listeners.py`는 선택한 SDK의 Linux 헤더를 전처리하고 콜백 전체를 구현한다.
현재 공개 API 정의에서 23개 인터페이스·181개 콜백을 확인했다. 인증, 회의 상태, 참가자, 오디오,
녹음, 영상 상태, 화면 공유 상태, 채팅, 대기실, 알림, 회의 설정, breakout, webinar,
암호화, AI Companion 상태, 오디오 장치 변경, 네트워크 알림을 포함한다. 해당 기능을 켜거나 내용을 전송하지 않는다.
mixed·share·interpreter raw audio 콜백도 메타데이터를 기록하며, 화자 WAV는 one-way PCM만 사용한다.

`sdk.callback_inventory`에 버전별 목록, `sdk.event_coverage`에 controller 제공 여부·등록 결과를 기록한다.
`sdk.callback`은 인터페이스명·콜백명·복사한 인자를 보존한다. 참가자·오디오 상태·채팅 내용·공유 상태 등
주요 데이터는 JSON으로 복사하고, 수명이 짧은 SDK action handler는 존재 여부만 기록한다.
지원하지 않는 객체 payload는 `opaque: true`로 표시한다. SDK가 제공하지 않거나 등록에 실패한 기능의
이벤트까지 수집했다고 주장하지 않는다. 영상·화면 공유 미디어 저장은 구현 범위 밖이다.

공개 헤더로 로컬에서 adapter/main을 컴파일하고 SDK 응답 대역 테스트를 실행할 수도 있다.
Python 3·GLib 개발 패키지가 추가로 필요하다. SDK 바이너리 링크는 별도다.

```sh
cmake -S worker -B build -DZOOM_API_HEADERS="$PWD/vendor/zoom-api/h"
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

## 건별 전달

`.env`에 `ZOOM_CHUNK_ENDPOINT`를 입력한다. 필요하면 `ZOOM_CHUNK_BEARER_TOKEN`도 설정한다.
endpoint는 완성된 WAV와 메타데이터를 건별로 받는 외부 API 주소다.
Bearer token은 해당 수신 API가 발급·검증하는 인증 토큰이며 Zoom 자격증명과 무관하다.
값에 `Bearer ` 접두사는 넣지 않는다. 전송기가 `Authorization: Bearer ...`를 붙인다.
endpoint를 비워 두면 로컬 파일 저장만 사용한다. 수신 API에 인증이 없으면 token도 비워 둔다.
HTTPS를 사용하며 로컬 테스트에 한해 loopback HTTP를 허용한다.

```sh
ruby bin/zoom-bot deliver SESSION_ID
ruby bin/zoom-bot deliver SESSION_ID --watch
```

`deliver`는 현재 준비된 청크를 보내고 종료한다. `--watch`는 250ms 간격으로 새 이벤트를 확인하며
타임아웃·408·429·5xx에 최대 60초 간격까지 재시도한다. 다른 HTTP 실패는 자동으로 건너뛰지 않는다.
캡처 프로세스와 별개로 실행하므로 회의가 끝난 후에도 전달을 계속할 수 있다.

POST 형식은 multipart다. `metadata`는 `audio.chunk_ready` 이벤트 전체를 담는 JSON이고,
`audio`는 화자·시간범위 파일명을 유지한 WAV다. `Idempotency-Key`는 `chunk_id`다.
전송기는 완성된 JSONL 행만 읽으며 WAV 저장·마감이 끝난 청크만 보낸다.
2xx 응답 후 커서를 기록한다. 응답 수신과 커서 저장 사이에 중단되면 같은 청크가 다시 전송될 수 있으므로
수신 시스템은 `chunk_id`로 중복을 제거해야 한다. 원본 WAV는 전달 성공 후에도 보존한다.

## 검증

```sh
ruby test/run.rb
```

토큰 서명·갱신, 응답 오류 비노출, 실제 host_id 선택, 잘못된 회의 ID 거부,
세션 격리·파일 권한, Docker 인자 내 토큰 비노출, 실행 실패 시 파일 보존을 검증한다.
Zoom HTTP와 Docker는 테스트 대역을 사용하고, WAV multipart 전송은 임시 loopback 서버로 검증한다.
C++ 테스트는 동시 화자 분리, 30초 경계, 발화 이벤트, pre-roll·overlap, 실제 VAD,
재입장, 미수신 마감, 부분 프레임, 시간범위 파일명을 검증한다.
추가 검증: S2S만으로 ZAK 발급, SDK 인증 실패 시 입장 차단, 역할·녹음 권한 확인,
권한 철회·복원·재접속, SDK 버퍼 수명 종료 후 화자별 WAV 유지, active-audio 원본 이벤트,
구독 실패 시 rollback, 큐 넘침 오류, 봇만 퇴장.
컨테이너에서 컴파일·테스트와 PulseAudio 장치 초기화를 검증했다.
Linux ARM64 SDK 7.2.1 바이너리 링크와 실제 Zoom API 인증·공동 호스트 입장은 별도로 확인했다.
SDK의 `USERROLE_COHOST` 값은 2이며, `is_host=false`만으로 일반 참가자라고 판단하지 않는다.
실제 WAV와 두 사람의 화자별 캡처 검증은 raw 오디오 접근 오류가 해결된 뒤 진행해야 한다.
