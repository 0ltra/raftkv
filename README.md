# raftkv (working title)

A Redis-compatible (RESP) key-value store in C++20, built in phases toward Raft replication
and a deterministic failure-injection test harness.

**Status:** Phase 1 in progress. A working single-node server with an append-only file for persistence. Benchmarks against real Redis are still to do.

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

To keep data across restarts, add an append-only file:

```bash
./build/release/src/raftkv_server --port 6380 --aof data.aof --fsync everysec
```

Then, from another terminal:

```bash
redis-cli -p 6380 SET greeting hello
redis-cli -p 6380 GET greeting
```

## Supported commands

`PING [message]`, `GET key`, `SET key value`, `DEL key [key ...]`, `EXPIRE key seconds`,
`TTL key`, `INCR key`. Strings only.

## Persistence

With `--aof FILE`, every successful write command is appended to the file together with the
timestamp it ran at, and the server replays the file on startup.

- **Replies wait for the log.** Commands read in one batch are written (and fsynced, per policy)
  together, and only then are their replies sent. An `OK` means the write is as durable as the policy says.
- **`--fsync always`**: fdatasync before every acknowledgement. Nothing acknowledged is lost.
  **`everysec`** (default): about once a second; a crash can lose roughly the last second.
  **`no`**: the OS decides.
- **Deterministic replay.** Commands run with the clock pinned to the logged timestamp, so
  `EXPIRE k 10` rebuilds the original deadline instead of "10 seconds after the restart".
- **Checksummed records.** Each record has a length and a CRC32. Recovery stops at the first
  incomplete or corrupt record and cuts the file there, so a crash in the middle of a write is harmless.
  The server prints a warning if it discarded anything.
- **Fail-stop.** After any write or fsync error the log is marked failed for good (an fsync is never
  retried), writes are refused with `-MISCONF`, and clients are disconnected rather than sent an `OK`.

Not done yet: log rewriting/compaction (the file only grows), and recovery cannot tell a torn
tail from corruption in the middle of the file, so it cuts at the first bad record either way.

## Known limitations

- Inline commands are not supported, so `telnet` and `nc` will not work. Use `redis-cli`.
- No `SET` options (`EX`, `NX`), no log compaction, IPv4 only, Linux only.

## Roadmap

1. Single-node server: RESP parser, epoll loop, commands, key expiry, append-only file
2. Raft core as a pure deterministic state machine
3. Deterministic simulator with fault injection and invariant checks
4. (Optional) linearizability checker
5. Benchmarks and write-up
