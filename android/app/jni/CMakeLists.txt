cmake_minimum_required(VERSION 3.16)
project(SMASHTV C)

# SDL3 source goes in app/jni/SDL (the GitHub workflow / build-apk.sh put it there)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)
add_subdirectory(src)
