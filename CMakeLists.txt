cmake_minimum_required(VERSION 3.22)
project(SchwungMonomodule LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Link-time optimisation: bit-exact, ~3% faster on the Move (docs/PERF.md).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# Upstream Monomodule (GPLv3): the emulator core only. Its JUCE plugins, Library app and tests are not built.
set(MNM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/libs/monomodule")
include(cmake/dsp56300.cmake)

set(MNM_CORE "${MNM_ROOT}/src/core")
add_library(mnmcore STATIC
  ${MNM_CORE}/firmware/Firmware.cpp
  ${MNM_CORE}/dsp/Digibank.cpp
  ${MNM_CORE}/host/HostModel.cpp
  ${MNM_CORE}/host/Lfo.cpp
  src/engine/dsp/DspEngine.cpp
  src/engine/MonoVoice.cpp
)
# Our engine glue (src/engine) shadows upstream's DspEngine and MonoVoice; the rest of the core is upstream's.
target_include_directories(mnmcore PUBLIC src/engine ${MNM_CORE})
target_link_libraries(mnmcore PUBLIC dsp56kEmu)
target_compile_definitions(mnmcore PUBLIC MNM_OS_SYX="")

add_executable(mnm-bench tools/bench/mnm_bench.cpp)
target_link_libraries(mnm-bench PRIVATE mnmcore)

add_executable(mnm-golden tools/bench/mnm_golden.cpp)
target_link_libraries(mnm-golden PRIVATE mnmcore)

add_executable(mnm-disasm tools/disasm/mnm_disasm.cpp)
target_link_libraries(mnm-disasm PRIVATE mnmcore)

# ---- the modules (Linux / Move only)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Engine process: one per module instance, spawned by the plugin.
  add_executable(mnm-engine src/child/mnm_engine.cpp)
  target_include_directories(mnm-engine PRIVATE src/common)
  target_link_libraries(mnm-engine PRIVATE mnmcore pthread)

  # Plugins: loaded into MoveOriginal. Header-only use of the core (machine tables); no emulator, no JIT.
  foreach(variant one fx)
    if(variant STREQUAL "one")
      set(_name dsp)
      set(_fx 0)
    else()
      set(_name monomodule-fx)
      set(_fx 1)
    endif()
    add_library(mnm-plugin-${variant} SHARED src/plugin/mnm_plugin.cpp ${MNM_CORE}/library/MnmDump.cpp)
    set_target_properties(mnm-plugin-${variant} PROPERTIES OUTPUT_NAME ${_name} PREFIX "" CXX_VISIBILITY_PRESET hidden)
    target_compile_definitions(mnm-plugin-${variant} PRIVATE MNM_FX=${_fx})
    target_include_directories(mnm-plugin-${variant} PRIVATE src src/common ${MNM_CORE})
    target_link_options(mnm-plugin-${variant} PRIVATE -static-libstdc++ -static-libgcc)
  endforeach()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(mnm-hosttest tools/hosttest/mnm_hosttest.cpp)
  target_include_directories(mnm-hosttest PRIVATE src)
  target_link_libraries(mnm-hosttest PRIVATE dl pthread)
endif()
