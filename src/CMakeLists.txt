# RESP protocol library: parser + encoder. Pure code, no sockets.
add_library(raftkv_resp
    resp/parser.cpp
    resp/encoder.cpp
)
target_include_directories(raftkv_resp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftkv_resp PUBLIC raftkv_options)

# Key-value store library: clock interface + in-memory store with expiry.
add_library(raftkv_store
    store/clock.cpp
    store/store.cpp
)
target_include_directories(raftkv_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftkv_store PUBLIC raftkv_options)

# Command dispatcher: parsed command in, RESP reply out.
add_library(raftkv_commands
    commands/dispatcher.cpp
)
target_include_directories(raftkv_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftkv_commands PUBLIC raftkv_resp raftkv_store raftkv_options)
