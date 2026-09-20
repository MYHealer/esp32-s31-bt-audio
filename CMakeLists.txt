# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH not defined")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# IDF 6.2 + GCC 16 treats Bluedroid strncpy as -Werror=stringop-truncation.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)

# Windows CreateProcess() caps a single command line at 32767 chars. This tree
# lives under a doubly-nested unzip path (.../ESP32-S31-A2DP-Codecs-main/
# ESP32-S31-A2DP-Codecs-main/esp-idf-v6.1-codecs/), so every -I flag carries a
# ~105 char prefix. With 225 include dirs in main and 379 in the project_elf
# source, some compile lines reach ~50k chars and Ninja fails with
# "CreateProcess: The parameter is incorrect."
# Forcing response files makes Ninja write the flags to a .rsp file instead of
# passing them on the command line. Requires CMake >= 3.27 and Ninja >= 1.11
# (this project uses 3.30.2 / 1.12.1).
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files to avoid Windows command line length limits")

project(bt_audio)
