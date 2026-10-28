# RESP protocol library: parser + encoder. Pure code, no sockets.
add_library(raftkv_resp
    resp/parser.cpp
    resp/encoder.cpp
)
target_include_directories(raftkv_resp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftkv_resp PUBLIC raftkv_options)
