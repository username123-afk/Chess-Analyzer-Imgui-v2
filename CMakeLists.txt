cmake_minimum_required(VERSION 3.22)

project(chess_analyzer_overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(chess-analyzer-overlay
    src/main.cpp
    src/a_native_window_creator.cpp
    src/chess_core.cpp
    src/vision.cpp
    src/stockfish.cpp
    src/settings.cpp
    src/touch_input.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(chess-analyzer-overlay PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_options(chess-analyzer-overlay PRIVATE
    -fexceptions
    -frtti
    -O2
    -Wall
)

target_link_libraries(chess-analyzer-overlay PRIVATE
    android
    log
    EGL
    GLESv3
    dl
)
