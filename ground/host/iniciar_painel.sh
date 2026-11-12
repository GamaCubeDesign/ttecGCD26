#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/ground-aeronaves"
exec python3 -m ground.visualize --serve "$@"
