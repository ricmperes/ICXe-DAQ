cmake_minimum_required(VERSION 3.16)
project(ICXe-DAQ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(icxe-daq
  src/main.cpp
  src/config.cpp
  src/buffers.cpp
  src/coincidence.cpp
  src/writer.cpp
  src/digitizer.cpp
  src/self_test.cpp
)

target_include_directories(icxe-daq PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

find_library(CAENDIGITIZER_LIB CAENDigitizer REQUIRED)
find_library(CAENCOMM_LIB CAENComm REQUIRED)
find_library(CAENVME_LIB CAENVME REQUIRED)

target_link_libraries(icxe-daq PRIVATE
  ${CAENDIGITIZER_LIB}
  ${CAENCOMM_LIB}
  ${CAENVME_LIB}
)

target_compile_options(icxe-daq PRIVATE -Wall -Wextra -Wpedantic)
