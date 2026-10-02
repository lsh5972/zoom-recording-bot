cmake_minimum_required(VERSION 3.22)
project(zoom_bot LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
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
  src/audio_segmenter.cpp src/audio_pipeline.cpp src/recording_timeline.cpp
  src/join_config.cpp src/capture_runtime.cpp
)
target_include_directories(zoom_bot_audio PUBLIC include PRIVATE ${libfvad_SOURCE_DIR}/include)
target_link_libraries(zoom_bot_audio PUBLIC PkgConfig::SNDFILE nlohmann_json::nlohmann_json Threads::Threads PRIVATE fvad)
target_compile_options(zoom_bot_audio PRIVATE -Wall -Wextra -Wpedantic)
add_executable(zoom-bot-replay src/replay.cpp)
target_link_libraries(zoom-bot-replay PRIVATE zoom_bot_audio)

enable_testing()
add_executable(audio_tests tests/audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE zoom_bot_audio)
target_compile_definitions(audio_tests PRIVATE FVAD_FIXTURE="${libfvad_SOURCE_DIR}/tests/data/audio_tiny16.wav")
add_test(NAME audio_tests COMMAND audio_tests)
add_executable(capture_tests tests/capture_tests.cpp)
target_link_libraries(capture_tests PRIVATE zoom_bot_audio)
add_test(NAME capture_tests COMMAND capture_tests)

set(ZOOM_SDK_ROOT "" CACHE PATH "Official Linux SDK distribution containing h/ and libmeetingsdk.so")
set(ZOOM_API_HEADERS "" CACHE PATH "Public API headers for compile checks only; no runtime")
if(ZOOM_SDK_ROOT OR ZOOM_API_HEADERS)
  if(ZOOM_SDK_ROOT)
    set(SDK_HEADERS "${ZOOM_SDK_ROOT}/h")
  else()
    set(SDK_HEADERS "${ZOOM_API_HEADERS}")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(LISTENERS "${CMAKE_CURRENT_BINARY_DIR}/generated/sdk_listeners.hpp")
  set(SDK_STUBS "${CMAKE_CURRENT_BINARY_DIR}/generated/sdk_stubs.hpp")
  file(GLOB_RECURSE SDK_HEADER_FILES CONFIGURE_DEPENDS "${SDK_HEADERS}/*.h")
  add_custom_command(OUTPUT "${LISTENERS}" "${SDK_STUBS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_sdk_listeners.py"
      --compiler "${CMAKE_CXX_COMPILER}" --headers "${SDK_HEADERS}" --output "${LISTENERS}" --stubs "${SDK_STUBS}"
    DEPENDS ${SDK_HEADER_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_sdk_listeners.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/zoom_bot/sdk_headers.hpp"
    VERBATIM)
  add_library(zoom_bot_sdk OBJECT src/zoom_session.cpp "${LISTENERS}")
  target_include_directories(zoom_bot_sdk PUBLIC "${SDK_HEADERS}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(zoom_bot_sdk PUBLIC zoom_bot_audio)
  target_compile_options(zoom_bot_sdk PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Select the Linux-only declarations when checking public headers on macOS.
    target_compile_definitions(zoom_bot_sdk PUBLIC __linux__)
  endif()
  add_executable(sdk_tests tests/sdk_tests.cpp "${SDK_STUBS}")
  target_link_libraries(sdk_tests PRIVATE zoom_bot_sdk)
  target_compile_options(sdk_tests PRIVATE -Wno-unused-parameter)
  target_compile_definitions(sdk_tests PRIVATE FVAD_FIXTURE="${libfvad_SOURCE_DIR}/tests/data/audio_tiny16.wav")
  add_test(NAME sdk_tests COMMAND sdk_tests)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
  add_library(zoom_bot_main OBJECT src/main.cpp)
  target_link_libraries(zoom_bot_main PRIVATE zoom_bot_sdk PkgConfig::GLIB)
  if(ZOOM_SDK_ROOT)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
      message(FATAL_ERROR "The actual Zoom worker requires Linux; use Docker on macOS")
    endif()
    find_library(MEETINGSDK meetingsdk PATHS "${ZOOM_SDK_ROOT}" NO_DEFAULT_PATH REQUIRED)
    add_executable(zoom-bot-worker $<TARGET_OBJECTS:zoom_bot_main>)
    target_link_libraries(zoom-bot-worker PRIVATE zoom_bot_sdk "${MEETINGSDK}" PkgConfig::GLIB)
    set_target_properties(zoom-bot-worker PROPERTIES BUILD_RPATH "${ZOOM_SDK_ROOT};${ZOOM_SDK_ROOT}/qt_libs;${ZOOM_SDK_ROOT}/qt_libs/Qt/lib")
  else()
    message(STATUS "Zoom adapter compile check only: SDK binary/runtime not available")
  endif()
endif()
