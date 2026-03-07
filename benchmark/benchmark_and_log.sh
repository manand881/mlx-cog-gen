#!/usr/bin/env bash
# Runs the benchmark and tees output to benchmark.log in the project root.
# Usage: ./benchmark_and_log.sh [iterations]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="$SCRIPT_DIR/benchmark.log"

"$SCRIPT_DIR/benchmark.sh" "$@" 2>&1 | tee "$LOG_FILE"
