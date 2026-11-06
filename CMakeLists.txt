cmake_minimum_required(VERSION 3.24)

project(media-analyse
    VERSION 0.1.0
    DESCRIPTION "Media analysis tool - refactor of MediaAnalyse"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

enable_testing()

# Only the FFmpeg DLLs we actually link; never the whole bin/ (which contains
# ffmpeg.exe/ffplay.exe/ffprobe.exe and unused libraries).
if(MA_FFMPEG_RUNTIME_DLL_DIR)
    file(GLOB MA_FFMPEG_RUNTIME_DLLS
        "${MA_FFMPEG_RUNTIME_DLL_DIR}/avcodec-*.dll"
        "${MA_FFMPEG_RUNTIME_DLL_DIR}/avformat-*.dll"
        "${MA_FFMPEG_RUNTIME_DLL_DIR}/avutil-*.dll"
        "${MA_FFMPEG_RUNTIME_DLL_DIR}/swscale-*.dll"
        "${MA_FFMPEG_RUNTIME_DLL_DIR}/swresample-*.dll")
endif()

add_subdirectory(core)
add_subdirectory(infra)
add_subdirectory(worker)

# Qt provisioning must happen before tests (which links ma::bridge + Qt6::Widgets)
# and before bridge/app. Guarded so the non-app preset never pays for Qt.
if(MA_ENABLE_APP)
    find_package(Qt6 REQUIRED COMPONENTS Core Quick Widgets)
    qt_standard_project_setup(REQUIRES 6.5)
endif()

add_subdirectory(tests)
add_subdirectory(cli)

# app (QML frontend) + bridge are added once the Qt toolchain is provisioned.
if(MA_ENABLE_APP)
    add_subdirectory(bridge)
    add_subdirectory(app)
endif()
