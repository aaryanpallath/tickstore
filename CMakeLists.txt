cmake_minimum_required(VERSION 3.20)
project(tickstore CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(TICKSTORE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
if(TICKSTORE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(ZLIB REQUIRED)
find_package(SQLite3 REQUIRED)
# CMake >= 4.0 names the target SQLite3::SQLite3; older versions (e.g. Ubuntu 24.04's 3.28) use SQLite::SQLite3.
if(TARGET SQLite3::SQLite3)
  set(TICKSTORE_SQLITE SQLite3::SQLite3)
else()
  set(TICKSTORE_SQLITE SQLite::SQLite3)
endif()

add_library(tickstore src/source.cpp src/feed.cpp src/store.cpp src/analytics.cpp)
target_include_directories(tickstore PUBLIC include)
target_link_libraries(tickstore PUBLIC ZLIB::ZLIB)

foreach(tool ts_ingest ts_query ts_report ts_bench)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE tickstore ${TICKSTORE_SQLITE})
endforeach()

include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(tickstore_tests tests/test_main.cpp)
target_compile_options(tickstore_tests PRIVATE -Wno-conversion)
target_link_libraries(tickstore_tests PRIVATE tickstore GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(tickstore_tests)
