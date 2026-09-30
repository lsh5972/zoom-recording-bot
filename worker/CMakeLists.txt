cmake_minimum_required(VERSION 3.24)
project(zoom_bot_audio LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

FetchContent_Declare(libfvad
  URL https://codeload.github.com/dpirch/libfvad/tar.gz/532ab666c20d3cfda38bca63abbb0f152706c369
  URL_HASH SHA256=bf639e4a5bc37b1c90e047c42524e80a1d0cecadeb6e0a61a33580fca22a00ab
)
FetchContent_Declare(json
  URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
  URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
)
# libfvad's CMake minimum predates CMake 4; keep compatibility inside the dependency.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libfvad json)

add_library(zoom_bot_audio
  src/event_journal.cpp src/wav_writer.cpp src/speech_detector.cpp
  src/audio_segmenter.cpp src/audio_pipeline.cpp
)
target_include_directories(zoom_bot_audio PUBLIC include PRIVATE ${libfvad_SOURCE_DIR}/include)
target_link_libraries(zoom_bot_audio PUBLIC PkgConfig::SNDFILE nlohmann_json::nlohmann_json PRIVATE fvad)
target_compile_options(zoom_bot_audio PRIVATE -Wall -Wextra -Wpedantic)
add_executable(zoom-bot-replay src/replay.cpp)
target_link_libraries(zoom-bot-replay PRIVATE zoom_bot_audio)

enable_testing()
add_executable(audio_tests tests/audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE zoom_bot_audio)
target_compile_definitions(audio_tests PRIVATE FVAD_FIXTURE="${libfvad_SOURCE_DIR}/tests/data/audio_tiny16.wav")
add_test(NAME audio_tests COMMAND audio_tests)
