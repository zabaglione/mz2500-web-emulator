cmake_minimum_required(VERSION 3.20)
project(mz2500_web_emulator LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(mz2500_core STATIC
  core/banked_memory.cpp
  core/cmt.cpp
  core/d88.cpp
  core/dummy_ipl.cpp
  core/fdc_mb8877.cpp
  core/mz2500.cpp
  core/opn.cpp
  core/adpcm.cpp
  core/printer.cpp
  core/renderer.cpp
  core/sasi.cpp
  core/z80_sio.cpp
  vendor/z80/z80.c
  vendor/ymfm/ymfm_opn.cpp
  vendor/ymfm/ymfm_ssg.cpp
  vendor/ymfm/ymfm_adpcm.cpp
  vendor/ymfm/ymfm_opl.cpp
  vendor/ymfm/ymfm_pcm.cpp
)
target_include_directories(mz2500_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

if(EMSCRIPTEN)
  add_executable(mz2500w wasm/bindings.cpp)
  target_link_libraries(mz2500w PRIVATE mz2500_core)
  set_target_properties(mz2500w PROPERTIES SUFFIX ".js")
  target_link_options(mz2500w PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createMZ2500"
    # node is for the MCP server (mcp/), which hosts the same module headless
    "-sENVIRONMENT=web,node"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sEXPORTED_FUNCTIONS=_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32,UTF8ToString"
    "-O2"
  )
else()
  add_executable(mz2500w-cli cli/main.cpp)
  target_link_libraries(mz2500w-cli PRIVATE mz2500_core)

  # The unit tests are development-tree only: tools/export_public.sh leaves
  # tests/ behind, so configuring them there would fail on the first missing
  # source and take the CLI down with it.
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)

  add_executable(test_d88 tests/unit/test_d88.cpp)
  target_link_libraries(test_d88 PRIVATE mz2500_core)
  enable_testing()
  add_test(NAME d88 COMMAND test_d88)

  add_executable(test_fdc tests/unit/test_fdc.cpp)
  target_link_libraries(test_fdc PRIVATE mz2500_core)
  add_test(NAME fdc COMMAND test_fdc)

  add_executable(test_disk_io tests/unit/test_disk_io.cpp)
  target_link_libraries(test_disk_io PRIVATE mz2500_core)
  add_test(NAME disk_io COMMAND test_disk_io)
  set_tests_properties(disk_io PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  find_program(Z80ASM_EXECUTABLE z80asm)
  if(Z80ASM_EXECUTABLE)
    set(FDC_DRIVER_HARNESS_BIN
      ${CMAKE_CURRENT_BINARY_DIR}/fdc_driver_harness.bin)
    set(FDC_HIL_HARNESS_BIN
      ${CMAKE_CURRENT_BINARY_DIR}/fdc_hil_harness.bin)
    set(FDC_BATCH_HARNESS_BIN
      ${CMAKE_CURRENT_BINARY_DIR}/fdc_batch_harness.bin)
    set(MZSD_FDC_LOADER_HARNESS_BIN
      ${CMAKE_CURRENT_BINARY_DIR}/mzsd_fdc_loader_harness.bin)
    add_custom_command(
      OUTPUT ${FDC_DRIVER_HARNESS_BIN}
      COMMAND ${Z80ASM_EXECUTABLE}
        -I ${CMAKE_CURRENT_SOURCE_DIR}/..
        -o ${FDC_DRIVER_HARNESS_BIN}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/fdc_driver_harness.asm
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/fdc_driver_harness.asm
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/fdc_driver.inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/fdc_protocol.inc
      VERBATIM)
    add_custom_command(
      OUTPUT ${FDC_HIL_HARNESS_BIN}
      COMMAND ${Z80ASM_EXECUTABLE}
        -I ${CMAKE_CURRENT_SOURCE_DIR}/..
        -o ${FDC_HIL_HARNESS_BIN}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/fdc_hil_harness.asm
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/fdc_hil_harness.asm
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/remote_monitor_fdc_hil.inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/fdc_driver.inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/fdc_protocol.inc
      VERBATIM)
    add_custom_command(
      OUTPUT ${FDC_BATCH_HARNESS_BIN}
      COMMAND ${Z80ASM_EXECUTABLE}
        -I ${CMAKE_CURRENT_SOURCE_DIR}/..
        -o ${FDC_BATCH_HARNESS_BIN}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/fdc_batch_harness.asm
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/fdc_batch_harness.asm
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/remote_monitor_fdc_batch.inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/fdc_driver.inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/fdc/z80/fdc_protocol.inc
      VERBATIM)
    add_custom_command(
      OUTPUT ${MZSD_FDC_LOADER_HARNESS_BIN}
      COMMAND ${Z80ASM_EXECUTABLE}
        -I ${CMAKE_CURRENT_SOURCE_DIR}/..
        -o ${MZSD_FDC_LOADER_HARNESS_BIN}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/mzsd_fdc_loader_harness.asm
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/z80/mzsd_fdc_loader_harness.asm
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/mz2500/audio/z80/mzsd_fdc_loader.inc
      VERBATIM)
    add_custom_target(z80_fdc_driver_harness
      DEPENDS
        ${FDC_DRIVER_HARNESS_BIN}
        ${FDC_HIL_HARNESS_BIN}
        ${FDC_BATCH_HARNESS_BIN}
        ${MZSD_FDC_LOADER_HARNESS_BIN})
    add_executable(test_z80_fdc_driver tests/unit/test_z80_fdc_driver.cpp)
    add_dependencies(test_z80_fdc_driver z80_fdc_driver_harness)
    target_link_libraries(test_z80_fdc_driver PRIVATE mz2500_core)
    add_test(NAME z80_fdc_driver
      COMMAND test_z80_fdc_driver
        ${FDC_DRIVER_HARNESS_BIN}
        ${FDC_HIL_HARNESS_BIN}
        ${FDC_BATCH_HARNESS_BIN}
        ${MZSD_FDC_LOADER_HARNESS_BIN})
  endif()

  add_executable(test_keyboard tests/unit/test_keyboard.cpp)
  target_link_libraries(test_keyboard PRIVATE mz2500_core)
  add_test(NAME keyboard COMMAND test_keyboard)

  add_executable(test_key_schedule tests/unit/test_key_schedule.cpp)
  target_link_libraries(test_key_schedule PRIVATE mz2500_core)
  add_test(NAME key_schedule COMMAND test_key_schedule)

  add_executable(test_mouse tests/unit/test_mouse.cpp)
  target_link_libraries(test_mouse PRIVATE mz2500_core)
  add_test(NAME mouse COMMAND test_mouse)

  add_executable(test_mouse_wiring tests/unit/test_mouse_wiring.cpp)
  target_link_libraries(test_mouse_wiring PRIVATE mz2500_core)
  add_test(NAME mouse_wiring COMMAND test_mouse_wiring)

  add_executable(test_palette tests/unit/test_palette.cpp)
  target_link_libraries(test_palette PRIVATE mz2500_core)
  add_test(NAME palette COMMAND test_palette)

  add_executable(test_vram_wait tests/unit/test_vram_wait.cpp)
  target_link_libraries(test_vram_wait PRIVATE mz2500_core)
  add_test(NAME vram_wait COMMAND test_vram_wait)

  add_executable(test_emm tests/unit/test_emm.cpp)
  target_link_libraries(test_emm PRIVATE mz2500_core)
  add_test(NAME emm COMMAND test_emm)

  add_executable(test_adpcm tests/unit/test_adpcm.cpp)
  target_link_libraries(test_adpcm PRIVATE mz2500_core)
  add_test(NAME adpcm COMMAND test_adpcm)

  add_executable(test_opn tests/unit/test_opn.cpp)
  target_link_libraries(test_opn PRIVATE mz2500_core)
  add_test(NAME opn COMMAND test_opn)

  add_executable(test_gcrtc tests/unit/test_gcrtc.cpp)
  target_link_libraries(test_gcrtc PRIVATE mz2500_core)
  add_test(NAME gcrtc COMMAND test_gcrtc)

  add_executable(test_z80_sio tests/unit/test_z80_sio.cpp)
  target_link_libraries(test_z80_sio PRIVATE mz2500_core)
  add_test(NAME z80_sio COMMAND test_z80_sio)

  add_executable(test_sio_wiring tests/unit/test_sio_wiring.cpp)
  target_link_libraries(test_sio_wiring PRIVATE mz2500_core)
  add_test(NAME sio_wiring COMMAND test_sio_wiring)

  add_executable(test_cmt tests/unit/test_cmt.cpp)
  target_link_libraries(test_cmt PRIVATE mz2500_core)
  add_test(NAME cmt COMMAND test_cmt)

  add_executable(test_printer tests/unit/test_printer.cpp)
  target_link_libraries(test_printer PRIVATE mz2500_core)
  add_test(NAME printer COMMAND test_printer)

  add_executable(test_sasi tests/unit/test_sasi.cpp)
  target_link_libraries(test_sasi PRIVATE mz2500_core)
  add_test(NAME sasi COMMAND test_sasi)

  add_executable(test_compat tests/unit/test_compat.cpp)
  target_link_libraries(test_compat PRIVATE mz2500_core)
  add_test(NAME compat COMMAND test_compat)

  endif()
endif()
