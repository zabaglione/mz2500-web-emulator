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
endif()
