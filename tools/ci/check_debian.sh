#!/bin/bash
# make check in Debian 13 — the Pi's distribution and its GCC 14 — on a copy
# of the tree, so the source can be mounted read-only and nothing is written
# back to it. Run by the CI (.github/workflows/check.yml) and by make
# check-debian:
#
#   docker run --rm -v "$PWD":/src:ro debian:trixie bash /src/tools/ci/check_debian.sh
#
# A warning only GCC 14 raises fails here, as it would in make pi-check.
set -e
src=${1:-/src}
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake python3 poppler-utils git ca-certificates >/dev/null
cp -a "$src" /tmp/ttec
cd /tmp/ttec
git config --global --add safe.directory /tmp/ttec
rm -rf build build-asan          # configured for the host's paths
gcc --version | head -1
make check
