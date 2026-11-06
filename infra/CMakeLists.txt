# infra - FFmpeg-dependent layer. Pure C++20, no Qt.
set(MA_FFMPEG_ROOT "" CACHE PATH "FFmpeg SDK root (contains include/ and lib/)")

find_path(MA_FFMPEG_INCLUDE_DIR libavformat/avformat.h
          HINTS ${MA_FFMPEG_ROOT}/include ${MA_FFMPEG_ROOT})
find_library(MA_FFMPEG_LIB_AVFORMAT NAMES avformat
             HINTS ${MA_FFMPEG_ROOT}/lib ${MA_FFMPEG_ROOT}/bin)
find_library(MA_FFMPEG_LIB_AVCODEC NAMES avcodec
             HINTS ${MA_FFMPEG_ROOT}/lib ${MA_FFMPEG_ROOT}/bin)
find_library(MA_FFMPEG_LIB_AVUTIL NAMES avutil
             HINTS ${MA_FFMPEG_ROOT}/lib ${MA_FFMPEG_ROOT}/bin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MAFFmpeg REQUIRED_VARS
    MA_FFMPEG_INCLUDE_DIR MA_FFMPEG_LIB_AVFORMAT MA_FFMPEG_LIB_AVCODEC MA_FFMPEG_LIB_AVUTIL)

add_library(ma_infra STATIC
    src/Interrupt.cpp
    src/ProbeAnalyzer.cpp
    src/CrashHandler.cpp
)
add_library(ma::infra ALIAS ma_infra)

target_include_directories(ma_infra PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MA_FFMPEG_INCLUDE_DIR}
)
target_link_libraries(ma_infra PUBLIC
    ma::core
    ${MA_FFMPEG_LIB_AVFORMAT}
    ${MA_FFMPEG_LIB_AVCODEC}
    ${MA_FFMPEG_LIB_AVUTIL}
)
target_compile_features(ma_infra PUBLIC cxx_std_20)

# CrashHandler needs DbgHelp for MiniDumpWriteDump on Windows. A public link
# propagates to every consumer (CLI, worker, GUI) so any of them can dump.
if(WIN32)
    target_link_libraries(ma_infra PUBLIC dbghelp)
endif()

if(MSVC)
    target_compile_options(ma_infra PRIVATE /W4 /permissive-)
else()
    target_compile_options(ma_infra PRIVATE -Wall -Wextra -Wpedantic)
endif()
