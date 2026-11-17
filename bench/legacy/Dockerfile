# Benchmark image for the Fall 2025 pipeline (README_UPDATES.md §11.5).
#
# Contains everything except Postgres, which runs in its own container:
#   * readsb (pinned), standing in for dump1090 on the Pi: decodes Beast and serves SBS-1
#   * the Fall 2025 Python streamer's dependencies (ads-b-receiver/requirements.txt)
#   * the Fall 2025 Node.js web server's dependencies (ads/package-lock.json)
#   * a C++ toolchain to build core/ (adsb-synth, adsb-replay) at run time
# The project source is mounted read-only at /repo; nothing in it is modified.

FROM ubuntu:24.04

ARG READSB_TAG=v3.16.21
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      build-essential pkg-config git ca-certificates \
      libncurses-dev zlib1g-dev libzstd-dev \
      cmake ninja-build g++ \
      python3 python3-venv \
      nodejs npm \
      procps netcat-openbsd \
  && rm -rf /var/lib/apt/lists/*

# readsb, pinned to a release tag, network-only build (no SDR libraries).
RUN git clone --depth 1 --branch ${READSB_TAG} https://github.com/wiedehopf/readsb.git /opt/readsb-src \
  && make -C /opt/readsb-src -j"$(nproc)" RTLSDR=no \
  && install -m 755 /opt/readsb-src/readsb /usr/local/bin/readsb \
  && git -C /opt/readsb-src rev-parse HEAD > /opt/readsb.commit \
  && rm -rf /opt/readsb-src

# Fall 2025 streamer dependencies, exactly as pinned in its requirements.txt,
# plus numpy for the analysis script only.
COPY ads-b-receiver/requirements.txt /opt/legacy/requirements.txt
RUN python3 -m venv /opt/venv \
  && /opt/venv/bin/pip install --quiet --no-cache-dir -r /opt/legacy/requirements.txt numpy==1.26.4

# Fall 2025 web server dependencies, from its lockfile.
COPY ads/package.json ads/package-lock.json /opt/legacy/ads/
RUN cd /opt/legacy/ads && npm ci --omit=dev --no-audit --no-fund --loglevel=error

ENV PATH=/opt/venv/bin:$PATH
WORKDIR /repo
