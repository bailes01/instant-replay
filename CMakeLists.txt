cmake_minimum_required(VERSION 3.20)
project(InstantReplay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(src third_party)

add_executable(InstantReplay
    src/main.cpp
    src/config.cpp
    src/dxgi_capture.cpp
    src/nvenc_encoder.cpp
    src/audio_capture.cpp
    src/replay_buffer.cpp
    src/mp4_muxer.cpp
    src/uploader.cpp
    src/tray_app.cpp
)

target_compile_definitions(InstantReplay PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

target_link_libraries(InstantReplay PRIVATE
    d3d11
    dxgi
    dwmapi
    mmdevapi
    mfplat
    mfuuid
    mf
    winhttp
    shlwapi
    shell32
    ole32
    uuid
    avrt
    user32
    gdi32
)

if (MSVC)
    target_compile_options(InstantReplay PRIVATE /W4 /O2)
else()
    target_compile_options(InstantReplay PRIVATE -O3 -Wall -Wextra -Wno-missing-field-initializers)
    target_link_options(InstantReplay PRIVATE -static -mwindows -s)
endif()
