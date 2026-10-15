#!/bin/bash
# Runs inside the toolchain image (see run_build.sh): compile + link, write the stripped binary to /out.
set -euo pipefail
JOBS=${JOBS:-64}
LTO_JOBS=${LTO_JOBS:-48}
# Bound ThinLTO's own parallelism (it defaults to all cores, and the link needs ~15 GB + ~0.5 GB per job).
printf '#!/bin/sh\nexec /usr/bin/ld.lld-20 "$@" --thinlto-jobs=%s\n' "$LTO_JOBS" > /usr/local/bin/ld.lld-jobs
chmod +x /usr/local/bin/ld.lld-jobs
mkdir -p /build/ClickHouse/build && cd /build/ClickHouse/build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
    -DENABLE_TESTS=OFF -DCOMPILER_CACHE=disabled -DLLD_PATH=/usr/local/bin/ld.lld-jobs ..
ninja -j "$JOBS" clickhouse
llvm-objcopy-20 --strip-debug programs/clickhouse /out/clickhouse
/out/clickhouse --version
