# mmi-plot - runs tools/plot_stream_perf.py (car-run logs -> PNG plots,
# matplotlib). `make plot RUN_DIR=...` mounts the run directory at /run.
FROM ghcr.io/astral-sh/uv:0.5.11-python3.12-bookworm-slim

ENV UV_CACHE_DIR=/tmp/uv-cache MPLCONFIGDIR=/tmp/mpl
COPY plot_stream_perf.py /opt/
# Resolve the script's pinned dependencies at build time.
RUN uv run --script /opt/plot_stream_perf.py --check x \
 && mkdir -p /tmp/mpl && chmod -R a+rwX /tmp/uv-cache /tmp/mpl
ENTRYPOINT ["uv", "run", "--script", "/opt/plot_stream_perf.py"]
