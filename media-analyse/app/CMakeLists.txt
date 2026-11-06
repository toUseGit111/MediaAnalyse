find_package(Qt6 REQUIRED COMPONENTS Core Quick Widgets)
qt_standard_project_setup(REQUIRES 6.5)

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

# FFmpeg runtime DLLs (only the linked ones).
if(MA_FFMPEG_RUNTIME_DLLS)
    add_custom_command(TARGET media-analyse POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MA_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:media-analyse>"
        COMMENT "Copying FFmpeg runtime DLLs"
    )
endif()

# MinGW C++ runtime DLLs (libgcc/libstdc++/libwinpthread) from the toolchain.
if(MINGW AND NOT MA_STATIC_APP_RUNTIME)
    get_filename_component(MA_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET media-analyse POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MA_COMPILER_DIR}/libgcc_s_seh-1.dll"
                "${MA_COMPILER_DIR}/libstdc++-6.dll"
                "${MA_COMPILER_DIR}/libwinpthread-1.dll"
                "$<TARGET_FILE_DIR:media-analyse>"
        COMMENT "Copying MinGW runtime DLLs"
    )
endif()

if(WIN32)
    set_target_properties(media-analyse PROPERTIES WIN32_EXECUTABLE ON)
endif()
