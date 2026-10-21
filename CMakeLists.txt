cmake_minimum_required(VERSION 3.21)
project(FlashFind VERSION 0.2.1 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # 静态 CRT，单 exe 发布

# PCRE2（BSD，见 THIRD_PARTY_NOTICES.md）：静态库，仅 UTF-16 宽度，启用 JIT
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_8 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_UNICODE ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBZ OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBBZ2 OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBEDIT OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_LIBREADLINE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pcre2
    GIT_REPOSITORY https://github.com/PCRE2Project/pcre2.git
    GIT_TAG pcre2-10.44
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(pcre2)

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
    src/Options.cpp src/Options.h
    src/FileOps.cpp src/FileOps.h
    src/NtfsSource.cpp src/NtfsSource.h src/Service.cpp
    src/MftIndexer.cpp
    src/Usn.cpp src/Usn.h
    resources/FlashFind.rc
)

target_compile_definitions(FlashFind PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 FLASHFIND_VERSION=L"${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(FlashFind PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(FlashFind PRIVATE $<$<CONFIG:Release>:/Zi>)   # Release 也生成 PDB（不随包发布），便于解析崩溃栈
    target_link_options(FlashFind PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
    target_link_options(FlashFind PRIVATE /MANIFEST:NO)  # 使用 rc 内嵌的 app.manifest
endif()
target_link_libraries(FlashFind PRIVATE pcre2-16-static comctl32 uxtheme dwmapi shell32 shlwapi ole32 d2d1 dwrite cabinet psapi advapi32)

# 非管理员自检（-pipetest 需要 NTFS 卷，不纳入 ctest）
enable_testing()
add_test(NAME initest COMMAND FlashFind -initest)
add_test(NAME incrtest COMMAND FlashFind -incrtest)
add_test(NAME wildtest COMMAND FlashFind -wildtest ${CMAKE_BINARY_DIR}/no_such.db)
add_test(NAME sorttest COMMAND FlashFind -sorttest ${CMAKE_BINARY_DIR}/no_such.db)
add_test(NAME syntaxtest COMMAND FlashFind -syntaxtest ${CMAKE_BINARY_DIR}/no_such.db)
add_test(NAME regextest COMMAND FlashFind -regextest ${CMAKE_BINARY_DIR}/no_such.db)
add_test(NAME incrtest_synthetic COMMAND FlashFind -incrtest ${CMAKE_BINARY_DIR}/no_such.db)
