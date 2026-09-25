cmake_minimum_required(VERSION 3.22)
project(SchwungMonomodule LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Upstream Monomodule (GPLv3): the emulator core only. Its JUCE plugins, Library app and tests are not built.
set(MNM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/monomodule")
include(cmake/dsp56300.cmake)

set(MNM_CORE "${MNM_ROOT}/src/core")
add_library(mnmcore STATIC
  ${MNM_CORE}/firmware/Firmware.cpp
  ${MNM_CORE}/dsp/DspEngine.cpp
  ${MNM_CORE}/dsp/Digibank.cpp
  ${MNM_CORE}/host/HostModel.cpp
  ${MNM_CORE}/host/Lfo.cpp
  ${MNM_CORE}/MonoVoice.cpp
)
target_include_directories(mnmcore PUBLIC ${MNM_CORE})
target_link_libraries(mnmcore PUBLIC dsp56kEmu)
target_compile_definitions(mnmcore PUBLIC MNM_OS_SYX="")

add_executable(mnm-bench tools/bench/mnm_bench.cpp)
target_link_libraries(mnm-bench PRIVATE mnmcore)

add_executable(mnm-golden tools/bench/mnm_golden.cpp)
target_link_libraries(mnm-golden PRIVATE mnmcore)
