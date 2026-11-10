cmake_minimum_required(VERSION 3.25)
project(Canivete VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: one self-contained exe, nothing to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(Canivete WIN32
    src/main.cpp
    src/settings.cpp
    src/settings_ui.cpp
    src/capture.cpp
    src/output.cpp
    res/canivete.rc
    res/app.manifest
)

target_compile_definitions(Canivete PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    CANIVETE_VERSION=L"${PROJECT_VERSION}")
target_compile_options(Canivete PRIVATE /W4 /utf-8 /permissive- /EHsc
    $<$<CONFIG:Release>:/GL /Gy>)
target_link_options(Canivete PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
target_link_libraries(Canivete PRIVATE
    user32 gdi32 shell32 ole32 windowscodecs dwmapi shcore shlwapi comctl32 advapi32)
