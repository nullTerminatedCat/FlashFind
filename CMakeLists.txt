cmake_minimum_required(VERSION 3.21)
project(FlashFind VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # 静态 CRT，单 exe 发布

add_executable(FlashFind WIN32
    src/main.cpp
    src/App.cpp src/App.h
    src/MainWindow.cpp src/MainWindow.h
    src/ResultList.cpp src/ResultList.h
    src/Index.cpp src/Index.h
    src/Db.cpp src/Db.h
    src/Fold.cpp src/Fold.h
    src/Log.cpp src/Log.h
    src/Search.cpp src/Search.h
    src/Settings.cpp src/Settings.h
    src/Theme.cpp src/Theme.h
    src/Tray.cpp src/Tray.h
    src/Hotkey.cpp src/Hotkey.h
    src/Dpi.h
    src/Util.h
    src/ServiceClient.cpp src/ServiceClient.h
    src/MftIndexer.cpp src/MftIndexer.h
    resources/FlashFind.rc
)

target_compile_definitions(FlashFind PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 FLASHFIND_VERSION=L"${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(FlashFind PRIVATE /utf-8 /W4 /permissive- /MP)
    target_link_options(FlashFind PRIVATE /MANIFEST:NO)  # 使用 rc 内嵌的 app.manifest
endif()
target_link_libraries(FlashFind PRIVATE comctl32 uxtheme dwmapi shell32 shlwapi ole32 d2d1 dwrite cabinet psapi)
