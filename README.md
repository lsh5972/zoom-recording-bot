# Zoom bot

우리 Zoom 계정이 주최하는 회의에 호스트 ZAK로 들어가는 봇.
Ruby는 인증·실행을 담당하고, 공식 Linux Meeting SDK를 사용하는 C++ worker가
실시간 음성·이벤트를 처리하는 구조다. RTMS, PostgreSQL, Rails는 사용하지 않는다.

## 현재 구현 상태

구현: S2S OAuth, 회의의 실제 호스트 조회, 해당 호스트의 ZAK 발급 요청,
SDK JWT 서명, 비공개 세션 파일 생성, Docker 실행·정지 명령.
SDK와 독립된 C++ 음성 엔진은 사용자별 PCM → WebRTC VAD → 발화별 WAV,
speech_on/off·청크 이벤트 기록까지 처리한다. Ruby 전송기는 완성된 WAV를 건별 POST하고
성공한 위치를 파일에 저장한다.
HTTP 오류에서 응답 원문을 출력하지 않고, 401은 토큰 갱신 후 한 번만 재시도한다.

**아직 Zoom에 들어가는 녹음 봇은 아니다.** SDK 연결 계층과 Docker 이미지는 미구현이다.
회의 입장, 실제 호스트 역할·녹음 권한 확인, SDK PCM·이벤트 콜백 연결은 후속 구현 대상이다.
음성 엔진과 전송기는 테스트 입력으로 검증했다.
실제 Zoom 자격증명으로 API나 회의 입장을 검증하지 않았다.

## 실행 환경과 설정

개발 검증 환경: Ruby 3.2.5. 실행 코드는 표준 라이브러리만 사용한다.
테스트에는 Minitest가 필요하다. `.ruby-version`을 인식하는 Ruby 환경에서 실행한다.

서로 다른 두 앱의 자격증명이 필요하다.

| 앱 | 환경변수 | 역할 |
| --- | --- | --- |
| Meeting SDK를 활성화한 General App | `ZOOM_SDK_CLIENT_ID`, `ZOOM_SDK_CLIENT_SECRET` | SDK JWT 서명 |
| 호스트와 같은 계정의 Server-to-Server OAuth App | `ZOOM_S2S_ACCOUNT_ID`, `ZOOM_S2S_CLIENT_ID`, `ZOOM_S2S_CLIENT_SECRET` | 회의 조회·호스트 ZAK 발급 |

S2S 앱에 필요한 범위:

- `meeting:read:meeting:admin`: `GET /v2/meetings/{meetingId}`
- `user:read:token:admin`: `GET /v2/users/{hostId}/token?type=zak`

SDK와 S2S의 Client ID/Secret은 서로 바꿔 쓰지 않는다.
ZAK는 사용자 인증 정보이며, 녹음 가능 여부는 입장 후 별도로 확인해야 한다.

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

`prepare`는 Zoom API로 회의와 호스트를 조회하고 새 ZAK·JWT를 준비한다.
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
실제 배포본을 확보한 뒤 SDK 버전·아키텍처·해시를 고정해야 한다.
공식 문서상 7.0부터 ARM64를 지원한다. 로컬 Docker는 Linux ARM64이므로
ARM64 SDK와 같은 아키텍처의 Ubuntu 이미지를 사용하고, x86 에뮬레이션을 전제로 삼지 않는다.

공식 Linux 소개에는 Meeting SDK의 봇·AI 노트테이커 미지원 정책 안내가 있다.
원시 녹음 샘플이 존재하는 것과 현재 서비스 용도의 지원 여부는 별개이므로
프로덕션 사용이 승인되었다고 간주하지 않는다.
[정책 안내](https://developers.zoom.us/docs/meeting-sdk/linux/)

## Zoom worker 연결 규약 — SDK 연결 미구현

`ZOOM_BOT_IMAGE`는 로컬에 빌드된 worker 이미지 이름이다.
`run`은 이미지 존재 여부부터 확인한 뒤 새 세션을 준비한다.

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

worker가 지켜야 할 순서:

1. SDK JWT 인증 → `userZAK`로 입장 → 실제 호스트/공동 호스트 역할 확인.
2. `CanStartRawRecording` 확인 → `StartRawRecording` → 사용자별 PCM 구독.
3. SDK 콜백에서 버퍼를 복사·큐잉하고, 별도 처리 루프에서 저장·VAD 수행.
4. 권한 철회·퇴장·종료 시 수집 중단, 진행 중 청크와 이벤트 마감.

호스트가 먼저 들어온 경우, 봇이 먼저 들어온 경우, 재접속한 경우를 각각 실제 회의로 검증해야 한다.
호스트 ZAK만으로 녹음 권한이나 기존 호스트와의 동시 입장이 보장된다고 가정하지 않는다.

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
- SDK의 active-audio 콜백 원본 보존은 SDK 연결 단계에서 구현한다.
- 재입장은 새 participant session으로 구분한다. 표시 이름으로 합치지 않는다.
- 이벤트는 `events.jsonl`에 순번·수집 시간·기록 시각과 함께 저장하고 매번 동기화한다.
  현재는 음성 엔진이 생성하는 발화·청크·참가자 수명주기·음성 포맷·누락 구간 이벤트를 기록한다.
  모든 SDK 이벤트는 **해당 봇에 노출되는 SDK 이벤트**를 뜻한다. SDK 연결 시 버전별 수집 목록이 필요하다.
- 외부 전사 구현은 범위 밖이다. 수신 측은 `chunk_id` 중복 처리와 overlap 구간을 고려해야 한다.

회의 출력 폴더 하나에 모든 화자의 WAV와 이벤트를 함께 저장한다. 화자별 하위 폴더는 만들지 않는다.

```text
runs/<session_id>/output/
  user-42-join-1__00:01:12-00:01:38__chunk-3.wav
  user-73-join-1__00:01:15-00:01:22__chunk-2.wav
  events.jsonl
  delivery.json
```

시간은 공통 수집 시작을 `00:00:00`으로 놓은 상대 시간이다. SDK 연결 전에는 실제 회의 시작과의
오프셋을 확인할 수 없다. 파일명의 초 단위 표시는 소수부를 버리며, JSON의 밀리초 값이 정확한 기준이다.
같은 초에 여러 청크가 생겨도 화자 세션과 순번으로 충돌하지 않는다.

## 음성 엔진 빌드·재생 검증

C++17, CMake 3.24 이상, pkg-config, libsndfile 개발 패키지가 필요하다.
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
실제 SDK 어댑터는 사용자별 PCM을 공통 타임라인으로 변환하고,
콜백 밖의 처리 루프에서 `AudioPipeline.consume`과 `advance`를 호출해야 한다.

## 건별 전달

`.env`에 `ZOOM_CHUNK_ENDPOINT`를 입력한다. 필요하면 `ZOOM_CHUNK_BEARER_TOKEN`도 설정한다.
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
실제 Zoom 회의·컨테이너 녹음 검증은 포함하지 않는다.
