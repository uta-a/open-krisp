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
  src/krisp_shim.cpp
  src/wasapi_io.cpp
)

target_link_libraries(openkrisp PRIVATE ole32 avrt)
