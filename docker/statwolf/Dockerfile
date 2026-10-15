# Runtime image: the official 25.4.5.24 server image with this branch's clickhouse binary.
# Built by run_build.sh (which compiles the binary first); see README.md.
# Pinned to the official image by digest: a local tag of the same name may be something else.
FROM clickhouse/clickhouse-server:25.4.5.24@sha256:69ced3e6e017fb652d77a0496858806f89fc54653dad8c3c6893939aa05e9833
COPY --chown=root:root --chmod=555 clickhouse /usr/bin/clickhouse
LABEL org.opencontainers.image.title="ClickHouse Server 25.4.5.24 (Statwolf sequenceMatch build)"
LABEL org.opencontainers.image.version="25.4.5.24-sequencematch-nomaxiter-timerange"
LABEL org.opencontainers.image.source="https://github.com/Statwolf/ClickHouse/tree/sequencematch-events-variants-25.4.5.24"
LABEL org.opencontainers.image.description="ClickHouse 25.4.5.24 + sequenceMatchEventsFirst/Last/All (#95318) + #118497 + #119871 + sequenceMatch max iterations returns no match + two-sided time conditions (?t>=X&&t<=Y)"
