#!/usr/bin/env bash
# Benchmarks raftkv_server against real Redis with identical redis-benchmark settings.
#
#   bench/run_bench.sh [path-to-raftkv_server]
#
# Needs: redis-server, redis-benchmark, redis-cli on PATH, and a release build of raftkv.
# Each cell is the median of RUNS runs, in requests per second. Results go to stdout as Markdown.

set -u

SERVER="${1:-build/release/src/raftkv_server}"
REQUESTS="${REQUESTS:-200000}"   # requests per test per run
CLIENTS="${CLIENTS:-50}"         # parallel connections
KEYSPACE="${KEYSPACE:-100000}"   # random keys, so we don't hammer a single key
RUNS="${RUNS:-3}"
TESTS="ping_mbulk,set,get,incr"
RAFT_PORT=6401
REDIS_PORT=6402

for tool in redis-server redis-benchmark redis-cli; do
    command -v "$tool" >/dev/null || { echo "missing: $tool (sudo apt-get install -y redis-server redis-tools)" >&2; exit 1; }
done
[ -x "$SERVER" ] || { echo "missing server binary: $SERVER (build the release preset first)" >&2; exit 1; }

WORK="$(mktemp -d)"
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

start_server() {  # $1 = raftkv|redis  $2 = port  $3 = mem|aof
    local kind=$1 port=$2 mode=$3
    rm -f "$WORK"/*.aof; rm -rf "$WORK/redis"; mkdir -p "$WORK/redis"
    if [ "$kind" = raftkv ]; then
        if [ "$mode" = aof ]; then
            "$SERVER" --port "$port" --aof "$WORK/raftkv.aof" --fsync everysec >/dev/null 2>&1 &
        else
            "$SERVER" --port "$port" >/dev/null 2>&1 &
        fi
    else
        if [ "$mode" = aof ]; then
            redis-server --port "$port" --save "" --dir "$WORK/redis" --appendonly yes --appendfsync everysec >/dev/null 2>&1 &
        else
            redis-server --port "$port" --save "" --appendonly no >/dev/null 2>&1 &
        fi
    fi
    PID=$!
    for _ in $(seq 1 50); do
        redis-cli -p "$port" PING >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "server $kind did not start" >&2; exit 1
}

stop_server() { kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""; }

# Prints "TEST rps" lines for one run.
one_run() {  # $1 = port  $2 = pipeline depth
    redis-benchmark -p "$1" -n "$REQUESTS" -c "$CLIENTS" -P "$2" -r "$KEYSPACE" -t "$TESTS" --csv 2>/dev/null \
        | tr -d '"' | awk -F, 'NR>0 && $2+0>0 {print $1, $2}'
}

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

# Runs RUNS times and prints "TEST median_rps" per test.
measure() {  # $1 = port  $2 = pipeline depth
    local i
    for i in $(seq 1 "$RUNS"); do one_run "$1" "$2"; done > "$WORK/raw.txt"
    for t in PING_MBULK SET GET INCR; do
        printf '%s %s\n' "$t" "$(grep "^$t " "$WORK/raw.txt" | cut -d' ' -f2 | median)"
    done
}

echo "# raftkv vs Redis"
echo
echo "- Date: $(date +%Y-%m-%d)"
echo "- Machine: $(uname -sm), $(nproc) CPUs"
echo "- Redis: $(redis-server --version | sed 's/.*v=\([0-9.]*\).*/\1/')"
echo "- redis-benchmark: $REQUESTS requests, $CLIENTS clients, $KEYSPACE random keys, median of $RUNS runs"
echo "- Client and server run on the same machine, so numbers are only comparable within this table."
echo

for mode in mem aof; do
    for depth in 1 16; do
        label="no persistence"; [ "$mode" = aof ] && label="append-only file, fsync everysec"
        echo "## $label, pipeline depth $depth"
        echo
        echo "| Command | raftkv (req/s) | Redis (req/s) | raftkv / Redis |"
        echo "|---|---:|---:|---:|"
        start_server raftkv "$RAFT_PORT" "$mode"; measure "$RAFT_PORT" "$depth" > "$WORK/a.txt"; stop_server
        start_server redis "$REDIS_PORT" "$mode"; measure "$REDIS_PORT" "$depth" > "$WORK/b.txt"; stop_server
        join "$WORK/a.txt" "$WORK/b.txt" | awk '{printf "| %s | %.0f | %.0f | %.0f%% |\n", $1, $2, $3, 100*$2/$3}'
        echo
    done
done
