cmake_minimum_required(VERSION 3.20)

project(streamcam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(streamcam STATIC
  src/backend_factory.cpp
  src/streamcam.cpp
)

target_include_directories(streamcam
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(APPLE)
  enable_language(OBJCXX)
  target_sources(streamcam PRIVATE
    src/platform/macos/backend.mm
  )
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  target_link_libraries(streamcam
    PRIVATE
      ${AVFOUNDATION_FRAMEWORK}
      ${COREMEDIA_FRAMEWORK}
      ${COREVIDEO_FRAMEWORK}
      ${FOUNDATION_FRAMEWORK}
  )
else()
  message(WARNING "Only the macOS backend is implemented in this initial version.")
endif()

add_executable(streamcam_list_devices examples/list_devices.cpp)
target_link_libraries(streamcam_list_devices PRIVATE streamcam)

add_executable(streamcam_capture_benchmark examples/capture_benchmark.cpp)
target_link_libraries(streamcam_capture_benchmark PRIVATE streamcam)
