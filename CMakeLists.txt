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
  core/d88.cpp
  core/dummy_ipl.cpp
  core/fdc_mb8877.cpp
  core/mz2500.cpp
  core/opn.cpp
  core/renderer.cpp
  vendor/z80/z80.c
  vendor/ymfm/ymfm_opn.cpp
  vendor/ymfm/ymfm_ssg.cpp
  vendor/ymfm/ymfm_adpcm.cpp
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
    "-sENVIRONMENT=web"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sEXPORTED_FUNCTIONS=_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32,UTF8ToString"
    "-O2"
  )
else()
  add_executable(mz2500w-cli cli/main.cpp)
  target_link_libraries(mz2500w-cli PRIVATE mz2500_core)

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

  add_executable(test_vram_wait tests/unit/test_vram_wait.cpp)
  target_link_libraries(test_vram_wait PRIVATE mz2500_core)
  add_test(NAME vram_wait COMMAND test_vram_wait)
endif()
