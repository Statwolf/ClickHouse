#!/bin/bash
# Builds the statwolf/clickhouse image of this branch from source, memory-capped:
#   toolchain image -> capped compile/link container -> runtime image.
# usage: docker/statwolf/run_build.sh [cpus] [memory]     (defaults: 64 cpus, 128g)
set -euo pipefail
cd "$(dirname "$0")"
source ./build.conf   # BRANCH, TAG
CPUS=${1:-64}
MEMORY=${2:-128g}
D=docker; docker ps >/dev/null 2>&1 || D="sudo -n docker"
echo "== toolchain image ($BRANCH)"
DOCKER_BUILDKIT=1 $D build --progress=plain -f Dockerfile.toolchain --build-arg BRANCH="$BRANCH" -t "ch-toolchain-$TAG" .
mkdir -p out && rm -f out/clickhouse
echo "== compile + link (capped: $MEMORY RAM, no extra swap, $CPUS cpus)"
$D rm -f "ch-build-$TAG" >/dev/null 2>&1 || true
$D run --rm --name "ch-build-$TAG" --memory="$MEMORY" --memory-swap="$MEMORY" --cpus="$CPUS" \
    -e JOBS="$CPUS" -e LTO_JOBS=$(( CPUS * 3 / 4 )) -e CHECK_FIRST="${CHECK_FIRST:-}" -v "$PWD/build.sh:/build.sh:ro" -v "$PWD/out:/out" \
    "ch-toolchain-$TAG" bash /build.sh
echo "== runtime image statwolf/clickhouse:$TAG"
DOCKER_BUILDKIT=1 $D build --progress=plain -f Dockerfile -t "statwolf/clickhouse:$TAG" out
echo "== DONE"
