# Statwolf build of ClickHouse 25.4.5.24 with:
#  - #95318  sequenceMatchEventsFirst / sequenceMatchEventsLast / sequenceMatchEventsAll
#  - #118497 fix for sequenceMatchEvents returning a non-matching chain (#118120)
#  - sequenceMatch family: exceeding max iterations returns "no match" instead of TOO_SLOW
#
# Build the binary first (from the repository root):
#   mkdir build && cd build
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DENABLE_TESTS=OFF ..
#   ninja clickhouse
# strip debug info (as in official images) into a clean build context:
#   mkdir ctx && llvm-objcopy-20 --strip-debug build/programs/clickhouse ctx/clickhouse
# then build the image:
#   docker build -f docker/statwolf/Dockerfile -t statwolf/clickhouse:25.4.5.24-sequencematch-nomaxiter ctx
FROM clickhouse/clickhouse-server:25.4.5.24

COPY --chown=root:root --chmod=555 clickhouse /usr/bin/clickhouse

LABEL org.opencontainers.image.title="ClickHouse Server 25.4.5.24 (Statwolf sequenceMatch build)"
LABEL org.opencontainers.image.version="25.4.5.24-sequencematch-nomaxiter"
LABEL org.opencontainers.image.source="https://github.com/Statwolf/ClickHouse/tree/sequencematch-events-variants-25.4.5.24"
LABEL org.opencontainers.image.description="ClickHouse 25.4.5.24 + sequenceMatchEventsFirst/Last/All (#95318) + #118497 fix + max-iterations returns no match"
