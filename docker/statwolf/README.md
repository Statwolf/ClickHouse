# statwolf/clickhouse:25.4.5.24-sequencematch-nomaxiter-timerange

ClickHouse 25.4.5.24 + sequenceMatchEventsFirst/Last/All (#95318) + #118497 + #119871 + sequenceMatch max iterations returns no match + two-sided time conditions (?t>=X&&t<=Y)

Branch `sequencematch-events-variants-25.4.5.24` of Statwolf/ClickHouse, on top of v25.4.5.24-stable.

## Build

    docker/statwolf/run_build.sh [cpus=64] [memory=128g]

Clones this branch from GitHub inside a toolchain image (clang/lld 20, Rust nightly-2025-07-07),
compiles and links it in a container capped at `memory` (the final ThinLTO link alone needs ~15 GB
plus ~0.5 GB per ThinLTO job), and builds `statwolf/clickhouse:25.4.5.24-sequencematch-nomaxiter-timerange` from
`clickhouse/clickhouse-server:25.4.5.24` with the stripped binary. About 1.5 hours with 64 cores.
Uses BuildKit (Docker >= 20.10).
