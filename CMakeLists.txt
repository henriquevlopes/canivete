cmake_minimum_required(VERSION 3.25)
project(Canivete VERSION 0.7.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: one self-contained exe, nothing to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(Canivete WIN32
    src/main.cpp
    src/settings.cpp
    src/settings_ui.cpp
    src/color.cpp
    src/editor.cpp
    src/capture.cpp
    src/output.cpp
    src/resizer.cpp
    src/renamer.cpp
    src/shell.cpp
    src/gif.cpp
    src/recorder.cpp
    src/alpha.cpp
    src/video.cpp
    src/audio.cpp
    src/maint.cpp
    src/maint_ops.cpp
    src/maint_info.cpp
    src/update.cpp
    src/upload.cpp
    src/theme.cpp
    src/fluent.cpp
    src/rmbg.cpp
    src/rmbg_model.cpp
    src/overlay.cpp
    src/trim.cpp
    src/ocr.cpp
    src/notice.cpp
    src/updatedlg.cpp
    res/canivete.rc
    res/app.manifest
)

target_compile_definitions(Canivete PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    CANIVETE_VERSION=L"${PROJECT_VERSION}")
target_compile_options(Canivete PRIVATE /W4 /utf-8 /permissive- /EHsc
    $<$<CONFIG:Release>:/GL /Gy>)
target_link_options(Canivete PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
target_link_libraries(Canivete PRIVATE
    user32 gdi32 shell32 ole32 windowscodecs dwmapi shcore shlwapi comctl32 advapi32
    mfplat mfreadwrite mfuuid mf wuguid oleaut32 setupapi uxtheme
    runtimeobject wbemuuid cfgmgr32 winhttp bcrypt crypt32 ws2_32 dxgi version delayimp)
# DLLs only some features need are loaded the first time they are used, not at start: the copy that
# sits in the tray maps less.
target_link_options(Canivete PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll
    /DELAYLOAD:winhttp.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:gdiplus.dll /DELAYLOAD:setupapi.dll)
# ONNX Runtime's C API header only: the DLL is downloaded on first use (src/rmbg.cpp).
target_include_directories(Canivete PRIVATE third_party/onnxruntime/include)
