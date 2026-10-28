cmake_minimum_required(VERSION 3.20)
project(raftkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One switch controls the sanitizer build:
#   none    - normal build
#   address - AddressSanitizer + UndefinedBehaviorSanitizer
#   thread  - ThreadSanitizer (cannot be combined with ASan)
set(RAFTKV_SANITIZER "none" CACHE STRING "Sanitizer to enable: none, address, thread")
set_property(CACHE RAFTKV_SANITIZER PROPERTY STRINGS none address thread)

add_library(raftkv_options INTERFACE)
target_compile_options(raftkv_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor)

if(RAFTKV_SANITIZER STREQUAL "address")
    target_compile_options(raftkv_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(raftkv_options INTERFACE -fsanitize=address,undefined)
elseif(RAFTKV_SANITIZER STREQUAL "thread")
    target_compile_options(raftkv_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(raftkv_options INTERFACE -fsanitize=thread)
elseif(NOT RAFTKV_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "RAFTKV_SANITIZER must be none, address or thread (got '${RAFTKV_SANITIZER}')")
endif()

message(STATUS "raftkv: sanitizer = ${RAFTKV_SANITIZER}")

# GoogleTest, pinned to a release tag so builds are reproducible.
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(src)

enable_testing()
include(GoogleTest)
add_subdirectory(tests)
