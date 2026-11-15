cmake_minimum_required(VERSION 3.21)
project(KeyShadow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network Concurrent)

# ---------------------------------------------------------------------------
# FFmpeg（预编译共享库）。可通过 -DFFMPEG_ROOT=... 覆盖。
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "D:/dev/ffmpeg-pkg/ffmpeg-9.0.2-full_build-shared" CACHE PATH "FFmpeg shared build root")

if(NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg not found at FFMPEG_ROOT=${FFMPEG_ROOT}")
endif()

file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")

function(keyshadow_ffmpeg_lib name)
    set(found_dll "")
    foreach(dll ${FFMPEG_DLLS})
        get_filename_component(dll_name "${dll}" NAME)
        if(dll_name MATCHES "^${name}-[0-9]+\\.dll$")
            set(found_dll "${dll}")
        endif()
    endforeach()

    add_library(FFmpeg::${name} SHARED IMPORTED)
    set_target_properties(FFmpeg::${name} PROPERTIES
        IMPORTED_IMPLIB "${FFMPEG_ROOT}/lib/${name}.lib"
        IMPORTED_LOCATION "${found_dll}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_ROOT}/include"
    )
endfunction()

foreach(fflib avcodec avutil swscale swresample)
    keyshadow_ffmpeg_lib(${fflib})
endforeach()

# ---------------------------------------------------------------------------
# 以静态库形式组织核心代码（便于单元测试复用）
# ---------------------------------------------------------------------------
set(KEYSHADOW_CORE_SOURCES
    src/core/adb/AdbProcess.cpp
    src/core/scrcpy/Protocol.cpp
    src/core/scrcpy/ScrcpyClient.cpp
    src/core/video/VideoFrame.cpp
    src/core/video/VideoDecoder.cpp
    src/core/video/VideoOptions.cpp
    src/core/video/RenderPolicy.cpp
    src/core/video/GpuFrameConverter.cpp
    src/core/audio/AudioDecoder.cpp
    src/core/audio/AudioPlayer.cpp
    src/core/input/RawInput.cpp
    src/core/input/InjectionThread.cpp
    src/core/engine/Engines.cpp
    src/core/profile/ProfileStore.cpp
    src/license/LicenseVerifier.cpp
)

set(KEYSHADOW_CORE_HEADERS
    src/core/adb/AdbProcess.h
    src/core/scrcpy/Protocol.h
    src/core/scrcpy/ScrcpyClient.h
    src/core/video/VideoFrame.h
    src/core/video/VideoDecoder.h
    src/core/video/VideoOptions.h
    src/core/video/RenderPolicy.h
    src/core/video/GpuFrameConverter.h
    src/core/audio/AudioDecoder.h
    src/core/audio/AudioPlayer.h
    src/core/input/RawInput.h
    src/core/input/InjectionThread.h
    src/core/engine/Engines.h
    src/core/profile/ProfileStore.h
    src/license/LicenseVerifier.h
    src/license/LicensePublicKey.h
)

add_library(keyshadow_core STATIC ${KEYSHADOW_CORE_SOURCES} ${KEYSHADOW_CORE_HEADERS})

target_include_directories(keyshadow_core PUBLIC src)

target_link_libraries(keyshadow_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets Qt6::Network Qt6::Concurrent
    FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
    ws2_32 winmm ole32 uuid dwmapi shcore d3d11 dxgi bcrypt
)

target_compile_definitions(keyshadow_core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(keyshadow_core PUBLIC /utf-8 /permissive- /W3)
endif()

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
set(KEYSHADOW_UI_SOURCES
    src/main.cpp
    src/ui/Theme.cpp
    src/ui/IconFactory.cpp
    src/ui/KeyNames.cpp
    src/ui/KeymapOverlay.cpp
    src/ui/TabStrip.cpp
    src/ui/MainWindow.cpp
    src/ui/LicenseDialog.cpp
    src/ui/pages/DevicePage.cpp
    src/ui/pages/ConsolePage.cpp
    src/ui/pages/SettingsPage.cpp
    src/ui/VideoWidget.cpp
    src/ui/GpuTextureInterop.cpp
    src/app/Session.cpp
    src/license/LicenseManager.cpp
)

set(KEYSHADOW_UI_HEADERS
    src/ui/Theme.h
    src/ui/IconFactory.h
    src/ui/KeyNames.h
    src/ui/KeymapOverlay.h
    src/ui/TabStrip.h
    src/ui/MainWindow.h
    src/ui/LicenseDialog.h
    src/ui/pages/DevicePage.h
    src/ui/pages/ConsolePage.h
    src/ui/pages/SettingsPage.h
    src/ui/VideoWidget.h
    src/ui/VideoPresentationSettings.h
    src/app/Session.h
    src/license/LicenseManager.h
)

qt_add_executable(KeyShadow WIN32 ${KEYSHADOW_UI_SOURCES} ${KEYSHADOW_UI_HEADERS})

target_link_libraries(KeyShadow PRIVATE keyshadow_core dbghelp)

qt_add_resources(KeyShadow "keyshadow_resources"
    PREFIX "/"
    BASE "resources"
    FILES
        resources/theme.qss
        resources/icons/check.svg
        resources/icons/check-disabled.svg
        resources/icons/app-icon.svg
        resources/icons/app-icon.ico
        resources/scrcpy-server.jar
)

# Windows 可执行文件图标（资源管理器、任务栏固定项）
if(WIN32)
    enable_language(RC)
    configure_file(resources/keyshadow.rc.in "${CMAKE_CURRENT_BINARY_DIR}/keyshadow.rc" @ONLY)
    target_sources(KeyShadow PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/keyshadow.rc")
endif()

# 将 FFmpeg 运行库复制到输出目录（与 exe 同级）
add_custom_command(TARGET KeyShadow POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:KeyShadow>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "$<TARGET_FILE_DIR:KeyShadow>"
    COMMENT "Copy FFmpeg runtime DLLs"
)

# ---------------------------------------------------------------------------
# 单元测试
# ---------------------------------------------------------------------------
add_executable(keyshadow_tests
    tests/test_core.cpp
)
target_link_libraries(keyshadow_tests PRIVATE keyshadow_core)
if(MSVC)
    target_compile_options(keyshadow_tests PRIVATE /utf-8)
endif()

add_executable(keyshadow_video_tests tests/test_video.cpp src/ui/VideoWidget.cpp
    src/ui/VideoWidget.h src/ui/GpuTextureInterop.cpp src/app/Session.cpp src/app/Session.h
    src/license/LicenseManager.cpp)
target_link_libraries(keyshadow_video_tests PRIVATE keyshadow_core)
qt_add_resources(keyshadow_video_tests "video_test_server"
    PREFIX "/" BASE "resources" FILES resources/scrcpy-server.jar)

# 无界面联调探针（连接真机验证协议/解码链路）
add_executable(keyshadow_probe tools/probe.cpp)
target_link_libraries(keyshadow_probe PRIVATE keyshadow_core)
add_custom_command(TARGET keyshadow_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:keyshadow_probe>"
)
