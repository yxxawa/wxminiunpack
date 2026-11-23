cmake_minimum_required(VERSION 3.24)

project(wxminiunpack VERSION 1.0.0 LANGUAGES CXX RC)

option(WEDECODE_BUILD_TESTS "Build native core tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "This desktop application targets Windows and WebView2.")
endif()

if(NOT MSVC)
  message(FATAL_ERROR "Build with MSVC 2022 or a compatible Visual Studio Build Tools installation.")
endif()

add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2" CACHE PATH "Path to the native WebView2 SDK")
set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_ROOT}/include")
set(WEBVIEW2_STATIC_LOADER "${WEBVIEW2_ROOT}/lib/x64/WebView2LoaderStatic.lib")

if(NOT EXISTS "${WEBVIEW2_INCLUDE_DIR}/WebView2.h")
  message(FATAL_ERROR "WebView2 headers are missing. Restore third_party/webview2 or set WEBVIEW2_ROOT.")
endif()
if(NOT EXISTS "${WEBVIEW2_STATIC_LOADER}")
  message(FATAL_ERROR "WebView2 x64 static loader is missing. Restore third_party/webview2 or set WEBVIEW2_ROOT.")
endif()

add_library(WebView2LoaderStatic STATIC IMPORTED GLOBAL)
set_target_properties(WebView2LoaderStatic PROPERTIES IMPORTED_LOCATION "${WEBVIEW2_STATIC_LOADER}")

add_library(wedecode_core STATIC
  native/src/common.cpp
  native/src/wxapkg.cpp
  native/src/reconstructor.cpp
  native/src/batch_runner.cpp
  native/src/wechat_cache_scanner.cpp
)
target_include_directories(wedecode_core PUBLIC native/include)
target_link_libraries(wedecode_core PUBLIC bcrypt shell32 shlwapi advapi32 winhttp psapi)

set_source_files_properties(native/resources/app.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/native/resources/app.html;${CMAKE_CURRENT_SOURCE_DIR}/native/resources/app.ico"
)

add_executable(wedecode WIN32
  native/src/app.cpp
  native/src/drop_target.cpp
  native/resources/app.rc
  native/resources/app.manifest
)
target_include_directories(wedecode PRIVATE native/include native/resources "${WEBVIEW2_INCLUDE_DIR}")
target_link_libraries(wedecode PRIVATE wedecode_core WebView2LoaderStatic ole32 oleaut32 user32 gdi32 comctl32 dwmapi)
set_target_properties(wedecode PROPERTIES OUTPUT_NAME "微信小程序一键反编译")

install(TARGETS wedecode RUNTIME DESTINATION .)

if(WEDECODE_BUILD_TESTS)
  enable_testing()
  add_executable(wedecode_core_tests native/tests/core_tests.cpp)
  target_compile_options(wedecode_core_tests PRIVATE /UNDEBUG)
  target_link_libraries(wedecode_core_tests PRIVATE wedecode_core)
  add_test(NAME wedecode_core_tests COMMAND wedecode_core_tests)
  add_executable(wedecode_reconstruction_tests native/tests/reconstruction_tests.cpp)
  target_link_libraries(wedecode_reconstruction_tests PRIVATE wedecode_core)
  add_test(NAME wedecode_reconstruction_tests COMMAND wedecode_reconstruction_tests)
endif()
