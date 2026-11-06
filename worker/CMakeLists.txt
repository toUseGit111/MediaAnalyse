# analyzer-worker - crash-isolated analysis kernel (Qt-free). Links only
# ma::infra (which pulls in core + FFmpeg + dbghelp). Co-located next to the GUI
# by app/CMakeLists.txt; also self-contained here for standalone/fuzz use.
add_executable(analyzer-worker worker_main.cpp)
target_link_libraries(analyzer-worker PRIVATE ma::infra)

# Static-link the MinGW C++ runtime for a portable single folder.
if(MINGW)
    target_link_options(analyzer-worker PRIVATE
        -static-libgcc -static-libstdc++ -static)
endif()

# FFmpeg runtime DLLs next to the exe.
if(MA_FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET analyzer-worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MA_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:analyzer-worker>"
        COMMENT "Copying FFmpeg runtime DLLs"
    )
endif()

# MinGW C++ runtime DLLs from the active toolchain.
if(MINGW AND NOT MA_STATIC_APP_RUNTIME)
    get_filename_component(MA_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET analyzer-worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MA_COMPILER_DIR}/libgcc_s_seh-1.dll"
                "${MA_COMPILER_DIR}/libstdc++-6.dll"
                "${MA_COMPILER_DIR}/libwinpthread-1.dll"
                "$<TARGET_FILE_DIR:analyzer-worker>"
        COMMENT "Copying MinGW runtime DLLs"
    )
endif()
