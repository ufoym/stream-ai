cmake_minimum_required(VERSION 3.20)

project(streamcam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STREAMCAM_BUILD_SHARED "Build a shared library for FFI examples" ON)
option(STREAMCAM_BUILD_PYTHON_MODULE "Build the CPython extension module" ON)

set(STREAMCAM_SOURCES
  src/backend_factory.cpp
  src/streamcam.cpp
)

set(STREAMCAM_PLATFORM_LIBS "")

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND STREAMCAM_SOURCES
    src/platform/macos/backend.mm
  )
  find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
  find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  list(APPEND STREAMCAM_PLATFORM_LIBS
    ${AVFOUNDATION_FRAMEWORK}
    ${COREMEDIA_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND STREAMCAM_SOURCES
    src/platform/linux/backend.cpp
  )
elseif(WIN32)
  list(APPEND STREAMCAM_SOURCES
    src/platform/windows/backend.cpp
  )
  list(APPEND STREAMCAM_PLATFORM_LIBS
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
  )
else()
  message(WARNING "streamcam does not have a native backend for this platform.")
endif()

function(streamcam_configure_target target_name)
  target_include_directories(${target_name}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  if(STREAMCAM_PLATFORM_LIBS)
    target_link_libraries(${target_name}
      PRIVATE
        ${STREAMCAM_PLATFORM_LIBS}
    )
  endif()
endfunction()

add_library(streamcam STATIC ${STREAMCAM_SOURCES})
streamcam_configure_target(streamcam)
set_target_properties(streamcam PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(STREAMCAM_BUILD_SHARED)
  add_library(streamcam_shared SHARED ${STREAMCAM_SOURCES})
  streamcam_configure_target(streamcam_shared)
  set_target_properties(streamcam_shared PROPERTIES OUTPUT_NAME streamcam)

  if(WIN32)
    set_target_properties(streamcam_shared PROPERTIES
      WINDOWS_EXPORT_ALL_SYMBOLS ON
      ARCHIVE_OUTPUT_NAME streamcam_shared
    )
  endif()
endif()

if(STREAMCAM_BUILD_PYTHON_MODULE)
  find_package(Python3 COMPONENTS Development.Module REQUIRED)

  Python3_add_library(streamcam_python MODULE WITH_SOABI
    src/python_module.cpp
  )
  target_link_libraries(streamcam_python
    PRIVATE
      Python3::Module
      streamcam
  )
  target_include_directories(streamcam_python
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  set_target_properties(streamcam_python PROPERTIES
    OUTPUT_NAME streamcam
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/examples
  )
endif()

add_executable(streamcam_list_devices examples/list_devices.cpp)
target_link_libraries(streamcam_list_devices PRIVATE streamcam)

add_executable(streamcam_capture_benchmark examples/capture_benchmark.cpp)
target_link_libraries(streamcam_capture_benchmark PRIVATE streamcam)
