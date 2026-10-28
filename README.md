# raftkv (working title)

A Redis-compatible (RESP) key-value store in C++20, built in phases toward Raft replication
and a deterministic failure-injection test harness.

**Status:** Phase 0 (project skeleton). No server code yet.

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

## Roadmap

1. Single-node server: RESP parser, epoll loop, commands, key expiry, append-only file
2. Raft core as a pure deterministic state machine
3. Deterministic simulator with fault injection and invariant checks
4. (Optional) linearizability checker
5. Benchmarks and write-up
