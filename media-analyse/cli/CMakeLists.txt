add_executable(media-analyse-cli main.cpp)
target_link_libraries(media-analyse-cli PRIVATE ma::infra)

# Portable single-file CLI: static-link the MinGW C++ runtime.
if(MINGW)
    target_link_options(media-analyse-cli PRIVATE
        -static-libgcc -static-libstdc++ -static)
endif()

# Copy the linked FFmpeg runtime DLLs next to the exe.
if(MA_FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET media-analyse-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MA_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:media-analyse-cli>"
        COMMENT "Copying FFmpeg runtime DLLs"
    )
endif()
