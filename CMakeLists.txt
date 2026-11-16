cmake_minimum_required(VERSION 3.15)
project(code_insight CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(code-insight
  src/main.cpp
  src/server.cpp
  src/http.cpp
  src/analyzer.cpp
  src/json_util.cpp
  src/poller_posix.cpp
  src/poller_win32.cpp
)

target_include_directories(code-insight PRIVATE include)

if(WIN32)
  target_link_libraries(code-insight PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(code-insight PRIVATE Threads::Threads)
endif()

if(MSVC)
  target_compile_options(code-insight PRIVATE /W3)
else()
  target_compile_options(code-insight PRIVATE -Wall -Wextra -O2)
endif()
