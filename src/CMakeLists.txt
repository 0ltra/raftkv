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

# Persistence: CRC32, append-only file, and the handler that logs writes.
add_library(raftkv_persistence
    persistence/crc32.cpp
    persistence/aof.cpp
    persistence/durable_handler.cpp
)
target_include_directories(raftkv_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftkv_persistence PUBLIC raftkv_commands raftkv_options)

# TCP server: epoll event loop (Linux only).
add_library(raftkv_net
    net/server.cpp
)
target_include_directories(raftkv_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftkv_net PUBLIC raftkv_commands raftkv_options)

# The server program.
add_executable(raftkv_server main.cpp)
target_link_libraries(raftkv_server PRIVATE raftkv_net raftkv_persistence)
