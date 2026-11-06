# core - pure C++20, no Qt, no FFmpeg. Reusable by CLI, tests and GUI.
add_library(ma_core STATIC
    src/UrlNormalizer.cpp
    src/Serialization.cpp
)
add_library(ma::core ALIAS ma_core)

target_include_directories(ma_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ma_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ma_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(ma_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
