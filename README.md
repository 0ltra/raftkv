# raftkv (working title)

A Redis-compatible (RESP) key-value store in C++20, built in phases toward Raft replication
and a deterministic failure-injection test harness.

**Status:** Phase 1 in progress. A working single-node server; no persistence yet.

## Requirements

- Linux (the server uses `epoll`). On macOS, use the dev container in `.devcontainer/`.
- CMake 3.20+, Ninja, GCC 13+ or Clang 16+

## Build and test

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Sanitizer builds (ASan and UBSan together, TSan separately; they cannot share a binary):

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

## Run the server

```bash
cmake --preset release && cmake --build --preset release
./build/release/src/raftkv_server --port 6380
```

Then, from another terminal:

```bash
redis-cli -p 6380 SET greeting hello
redis-cli -p 6380 GET greeting
```

## Supported commands

`PING [message]`, `GET key`, `SET key value`, `DEL key [key ...]`, `EXPIRE key seconds`,
`TTL key`, `INCR key`. Strings only.

## Known limitations

- Inline commands are not supported, so `telnet` and `nc` will not work. Use `redis-cli`.
- No `SET` options (`EX`, `NX`), no persistence yet, IPv4 only, Linux only.

## Roadmap

1. Single-node server: RESP parser, epoll loop, commands, key expiry, append-only file
2. Raft core as a pure deterministic state machine
3. Deterministic simulator with fault injection and invariant checks
4. (Optional) linearizability checker
5. Benchmarks and write-up
