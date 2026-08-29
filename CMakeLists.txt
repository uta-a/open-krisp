cmake_minimum_required(VERSION 3.20)
project(openkrisp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8 /EHsc /O2 /W3)
endif()

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(openkrisp
  src/main.cpp
  src/cli.cpp
  src/engine.cpp
  src/settings.cpp
  src/hotkey.cpp
  src/tui_screen.cpp
  src/tui_app.cpp
  src/krisp_shim.cpp
  src/wasapi_io.cpp
  res/openkrisp.rc
)

# .rc から openkrisp.ico を相対名で参照しているので、rc.exe に置き場所を教える
target_include_directories(openkrisp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/res)

target_link_libraries(openkrisp PRIVATE ole32 avrt)
