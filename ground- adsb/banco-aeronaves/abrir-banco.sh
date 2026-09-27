#!/usr/bin/env bash
set -euo pipefail
banco_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -m sqlite3 "$banco_dir/referencias.db"
