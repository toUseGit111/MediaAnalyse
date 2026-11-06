# bridge - thin Qt adapter layer. Only place allowed to touch core+infra AND Qt.
# Qt6::Widgets is required for QFileDialog's native Windows file picker.
find_package(Qt6 REQUIRED COMPONENTS Core Quick Widgets)

add_library(ma_bridge STATIC
    src/Bridge.cpp
    include/ma/bridge/Bridge.hpp
)
add_library(ma::bridge ALIAS ma_bridge)

target_include_directories(ma_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ma_bridge PUBLIC ma::infra Qt6::Core Qt6::Quick Qt6::Widgets)
target_compile_features(ma_bridge PUBLIC cxx_std_20)

set_target_properties(ma_bridge PROPERTIES AUTOMOC ON)
set_target_properties(ma_bridge PROPERTIES AUTOMOC_PATH_PREFIX OFF)

if(MSVC)
    target_compile_options(ma_bridge PRIVATE /W4 /permissive-)
else()
    target_compile_options(ma_bridge PRIVATE -Wall -Wextra -Wpedantic)
endif()
