#!/usr/bin/env bash
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/scripts/dev-env.sh"
exec python3 "$root/scripts/latency_comparison.py" "$@"
