# tests - serialization round-trip + IPC envelope checks. Links only ma_core
# (no FFmpeg, no Qt) so it builds and runs quickly as a CI smoke gate.
add_executable(ma_ser_test ser_test.cpp)
target_link_libraries(ma_ser_test PRIVATE ma::core)
target_compile_features(ma_ser_test PRIVATE cxx_std_20)

enable_testing()
add_test(NAME ser_test COMMAND ma_ser_test)

# Controller integration test (P2): needs Qt/bridge, so only with the app build.
if(MA_ENABLE_APP)
    add_executable(ma_controller_test controller_test.cpp)
    target_link_libraries(ma_controller_test PRIVATE ma::bridge Qt6::Widgets)
    set_target_properties(ma_controller_test PROPERTIES AUTOMOC ON)

    # Co-locate the worker + its runtime DLLs next to the test so the controller
    # can spawn it from applicationDirPath().
    add_custom_command(TARGET ma_controller_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:analyzer-worker>"
                "$<TARGET_FILE_DIR:ma_controller_test>"
        COMMENT "Copying analyzer-worker for controller_test")
    if(MA_FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ma_controller_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${MA_FFMPEG_RUNTIME_DLLS}
                    "$<TARGET_FILE_DIR:ma_controller_test>"
            COMMENT "Copying FFmpeg runtime DLLs for controller_test")
    endif()
    if(MINGW AND NOT MA_STATIC_APP_RUNTIME)
        get_filename_component(MA_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        add_custom_command(TARGET ma_controller_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MA_COMPILER_DIR}/libgcc_s_seh-1.dll"
                    "${MA_COMPILER_DIR}/libstdc++-6.dll"
                    "${MA_COMPILER_DIR}/libwinpthread-1.dll"
                    "$<TARGET_FILE_DIR:ma_controller_test>"
            COMMENT "Copying MinGW runtime DLLs for controller_test")
    endif()
endif()
