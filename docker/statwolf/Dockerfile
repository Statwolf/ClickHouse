# Runtime image: the official 25.4.5.24 server image with this branch's clickhouse binary.
# Built by run_build.sh (which compiles the binary first); see README.md.
FROM clickhouse/clickhouse-server:25.4.5.24
COPY --chown=root:root --chmod=555 clickhouse /usr/bin/clickhouse
LABEL org.opencontainers.image.title="ClickHouse Server 25.4.5.24 (Statwolf sequenceMatch build)"
LABEL org.opencontainers.image.version="25.4.5.24-sequencematch-fastmatch-timerange"
LABEL org.opencontainers.image.source="https://github.com/Statwolf/ClickHouse/tree/sequencematch-fast-matcher-25.4.5.24"
LABEL org.opencontainers.image.description="ClickHouse 25.4.5.24 + sequenceMatchEventsFirst/Last/All (#95318) + #118497 + #119871 + memoized sequenceMatch backtracking + two-sided time conditions (?t>=X&&t<=Y)"
