# raftkv

A Redis-compatible (RESP) key-value store in C++20, built in phases toward Raft replication
and a deterministic failure-injection test harness.

**Status:** Phase 1 (single-node server) complete. A working single-node server with an append-only file for persistence. Next: Raft core.

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

## Benchmark vs Redis

`bench/run_bench.sh` runs the same `redis-benchmark` workload against raftkv and real Redis 7.0.15
(50 clients, 200k requests, 100k random keys, median of 3 runs). Both run on one machine, inside a
Linux dev container (aarch64, 18 CPUs) on a Mac, so only compare numbers within a table.
Full output is in `bench/results.md`.

Pipeline depth 16, no persistence (requests per second):

| Command | raftkv | Redis | raftkv / Redis |
|---|---:|---:|---:|
| PING | 3,703,704 | 3,921,569 | 94% |
| SET | 2,197,802 | 2,500,000 | 88% |
| GET | 2,439,024 | 2,941,176 | 83% |
| INCR | 2,247,191 | 2,816,901 | 80% |

Pipeline depth 16, append-only file with fsync everysec on both:

| Command | raftkv | Redis | raftkv / Redis |
|---|---:|---:|---:|
| PING | 3,703,704 | 3,448,276 | 107% |
| SET | 1,449,275 | 1,449,275 | 100% |
| GET | 2,564,102 | 2,127,660 | 121% |
| INCR | 1,562,500 | 1,639,344 | 95% |

Without pipelining (depth 1) both servers land at roughly 255k-280k req/s on every command, with or
without the log, and raftkv is within 5% of Redis on all but one row. That mostly reflects the
client and the loopback round trip, not the servers, so the pipelined runs say more.

Caveats: results vary by 20% or more between runs (GET gains 38 points between the two tables
above although the AOF cannot affect reads), so read these as "same ballpark as Redis", not as a win or
a loss. Redis does far more per request (many data types, statistics, replication hooks).
