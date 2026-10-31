cmake_minimum_required(VERSION 3.16)
project(smashtv_native VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
option(SMASHTV_VENDOR_SDL "Build SDL3 from source instead of using the system copy" OFF)

if(NOT SMASHTV_VENDOR_SDL)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  include(FetchContent)
  message(STATUS "Using SDL3 from source")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_SDL3 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL")
  endif()
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.24
    GIT_SHALLOW TRUE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

set(SOURCES src/main.c src/romset.c src/touch.c)

if(APPLE)
  add_executable(smashtv MACOSX_BUNDLE ${SOURCES})
  set_target_properties(smashtv PROPERTIES
    OUTPUT_NAME "Smash TV"
    MACOSX_BUNDLE_BUNDLE_NAME "Smash T.V."
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.smashtv-native"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
elseif(WIN32)
  add_executable(smashtv WIN32 ${SOURCES})
else()
  add_executable(smashtv ${SOURCES})
endif()

target_link_libraries(smashtv PRIVATE SDL3::SDL3)

# Optional: compile YOUR ROMs into the program (-DSMASHTV_EMBED_ROMS=/path/to/smashtv.zip or folder).
# Builds made this way contain the ROM data; keep them on your own machines.
set(SMASHTV_EMBED_ROMS "" CACHE STRING "smashtv.zip or ROM folder to compile into the app")
if(SMASHTV_EMBED_ROMS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(EMBED_C ${CMAKE_CURRENT_BINARY_DIR}/embedded_roms.c)
  add_custom_command(OUTPUT ${EMBED_C}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_roms.py "${SMASHTV_EMBED_ROMS}" ${EMBED_C}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_roms.py "${SMASHTV_EMBED_ROMS}"
    COMMENT "Embedding ROMs into the app")
  target_sources(smashtv PRIVATE ${EMBED_C})
  target_compile_definitions(smashtv PRIVATE SMASHTV_EMBEDDED_ROMS=1)
endif()
if(MSVC)
  target_compile_options(smashtv PRIVATE /W3)
else()
  target_compile_options(smashtv PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(MINGW)
  target_link_options(smashtv PRIVATE -static -static-libgcc)
endif()
