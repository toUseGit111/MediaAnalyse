# Qt6 provisioning (find_package + qt_standard_project_setup) is hoisted to the
# root CMakeLists.txt under MA_ENABLE_APP, before any Qt target is declared.
qt_add_executable(media-analyse main.cpp)

# Register Main.qml at the module root so loadFromModule("MediaAnalyse","Main") works.
set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
# Theme.qml is a QML singleton (PRISM design tokens).
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(media-analyse
    URI MediaAnalyse
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/PrismButton.qml
        qml/SectionHead.qml
        qml/MetricCell.qml
        qml/StreamCard.qml
        qml/FrameGrid.qml
)

# QApplication (from QtWidgets) is required by the native file dialog.
target_link_libraries(media-analyse PRIVATE ma::bridge Qt6::Widgets)

# Ensure the worker is built (and its DLLs copied) before we co-locate it.
add_dependencies(media-analyse analyzer-worker)

# Qt runtime DLLs + QML modules next to the exe (green package).
# windeployqt resolves exactly the Qt DLLs/plugins/imports that are needed,
# so we deliberately do NOT copy the whole Qt bin directory here.

# Platform plugins + QML imports. windeployqt ships with Qt and is authoritative.
find_program(MA_WINDEPLOYQT windeployqt HINTS "${MA_QT_RUNTIME_DIR}" NO_DEFAULT_PATH)
if(MA_WINDEPLOYQT)
    add_custom_command(TARGET media-analyse POST_BUILD
        COMMAND "${MA_WINDEPLOYQT}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --no-translations
                --no-system-d3d-compiler
                "$<TARGET_FILE:media-analyse>"
        COMMENT "Running windeployqt (plugins + QML imports)"
    )
    # windeployqt assumes a bin/ layout and writes Prefix=..; our exe sits at the
    # package root, so overwrite it with the correct prefix.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf.dist" "[Paths]\nPrefix=.\n")
    add_custom_command(TARGET media-analyse POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/qt.conf.dist"
                "$<TARGET_FILE_DIR:media-analyse>/qt.conf"
        COMMENT "Fixing qt.conf prefix"
    )
endif()

# --- Deploy the runtime files the package needs, next to the GUI -------------
# These must track the *worker* build too, so they cannot be POST_BUILD steps of
# media-analyse: those only run when the GUI itself relinks, which left a stale
# analyzer-worker.exe behind after worker-only changes. An ALL target runs on
# every build, and copy_if_different keeps the no-op case cheap.
set(MA_APP_DEPLOY_FILES "$<TARGET_FILE:analyzer-worker>")
if(MA_FFMPEG_RUNTIME_DLLS)
    list(APPEND MA_APP_DEPLOY_FILES ${MA_FFMPEG_RUNTIME_DLLS})
endif()
if(MINGW AND NOT MA_STATIC_APP_RUNTIME)
    get_filename_component(MA_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND MA_APP_DEPLOY_FILES
        "${MA_COMPILER_DIR}/libgcc_s_seh-1.dll"
        "${MA_COMPILER_DIR}/libstdc++-6.dll"
        "${MA_COMPILER_DIR}/libwinpthread-1.dll")
endif()

add_custom_target(ma_deploy_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MA_APP_DEPLOY_FILES}
            "$<TARGET_FILE_DIR:media-analyse>"
    DEPENDS analyzer-worker media-analyse
    COMMENT "Deploying analyzer-worker + FFmpeg/MinGW runtime DLLs"
)

if(WIN32)
    set_target_properties(media-analyse PROPERTIES WIN32_EXECUTABLE ON)
endif()
