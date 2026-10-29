# raftkv vs Redis

- Date: 2026-10-29
- Machine: Linux aarch64, 18 CPUs
- Redis: 7.0.15
- redis-benchmark: 200000 requests, 50 clients, 100000 random keys, median of 3 runs
- Client and server run on the same machine, so numbers are only comparable within this table.

## no persistence, pipeline depth 1

| Command | raftkv (req/s) | Redis (req/s) | raftkv / Redis |
|---|---:|---:|---:|
| PING_MBULK | 277778 | 264550 | 105% |
| SET | 261097 | 268456 | 97% |
| GET | 259740 | 261438 | 99% |
| INCR | 255102 | 263505 | 97% |

## no persistence, pipeline depth 16

| Command | raftkv (req/s) | Redis (req/s) | raftkv / Redis |
|---|---:|---:|---:|
| PING_MBULK | 3703704 | 3921569 | 94% |
| SET | 2197802 | 2500000 | 88% |
| GET | 2439024 | 2941176 | 83% |
| INCR | 2247191 | 2816901 | 80% |

## append-only file, fsync everysec, pipeline depth 1

| Command | raftkv (req/s) | Redis (req/s) | raftkv / Redis |
|---|---:|---:|---:|
| PING_MBULK | 277392 | 269179 | 103% |
| SET | 258732 | 270636 | 96% |
| GET | 260756 | 269542 | 97% |
| INCR | 258398 | 270636 | 95% |

## append-only file, fsync everysec, pipeline depth 16

| Command | raftkv (req/s) | Redis (req/s) | raftkv / Redis |
|---|---:|---:|---:|
| PING_MBULK | 3703704 | 3448276 | 107% |
| SET | 1449275 | 1449275 | 100% |
| GET | 2564102 | 2127660 | 121% |
| INCR | 1562500 | 1639344 | 95% |

