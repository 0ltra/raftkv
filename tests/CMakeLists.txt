# Every *_test.cpp file becomes part of one test binary, "raftkv_tests".
# New test files are picked up automatically when CMake is re-run.
file(GLOB RAFTKV_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

find_package(Threads REQUIRED)

add_executable(raftkv_tests ${RAFTKV_TEST_SOURCES})
target_link_libraries(raftkv_tests PRIVATE
    raftkv_options raftkv_resp raftkv_store raftkv_commands raftkv_net
    Threads::Threads GTest::gtest_main)

# Registers each TEST() as its own CTest entry so failures name the exact test.
# The timeout turns a hung socket test into a failure instead of a stuck build.
gtest_discover_tests(raftkv_tests PROPERTIES TIMEOUT 60)
