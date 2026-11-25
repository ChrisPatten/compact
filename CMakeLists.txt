cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.local")
  list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
endif()
# Heap-tracing debug build (sdkconfig.trace): idf.py -B build-trace -D SDKCONFIG=build-trace/sdkconfig -D INTERCOM_TRACE=1 build
if(INTERCOM_TRACE)
  list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.trace")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# More invalid-area slots (default 32): the breathing ring invalidates a few dozen small tiles per frame; overflow
# turns the whole screen into one redraw.
idf_build_set_property(COMPILE_OPTIONS "-DLV_INV_BUF_SIZE=96" APPEND)
project(intercom_firmware)
