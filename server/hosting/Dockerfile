# shadNet on Ubuntu 24.04. UNTESTED as an image: no Docker daemon was available.
#
# Qt version: shadNet needs Qt >= 6.8. src/webapi_server.cpp uses the 6.8 QHttpServer API:
# bool bind(), setMissingHandler(ctx, ...) and QHttpServerResponder::sendResponse. Ubuntu 24.04's
# apt Qt is 6.4.2, and the build fails there (checked: a native build on 24.04 with
# qt6-base-dev/qt6-httpserver-dev stops in webapi_server.cpp, all other files compile).
# So Qt 6.8 LTS comes from aqtinstall (download.qt.io). Alternative: FROM debian:trixie and apt
# qt6-base-dev qt6-httpserver-dev (Qt 6.8.2); see the comment at the bottom.
#
# Build context is server/ (the folder above this one), with the protobuf submodule checked out:
#   git submodule update --init server/externals/protobuf
#   docker build -f server/hosting/Dockerfile -t shadnet:local server
# protobuf_FORCE_FETCH_DEPENDENCIES=ON fetches abseil while configuring, so the build needs network.

ARG QT_VERSION=6.8.3

FROM ubuntu:24.04 AS build
ARG QT_VERSION
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git cmake ninja-build g++ python3-pip python3-venv \
        libglib2.0-dev libgl-dev libssl-dev libpq-dev \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/aqt && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.2.1 \
    && /opt/aqt/bin/aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 \
         -m qthttpserver qtwebsockets -O /opt/qt \
    && rm -rf /opt/qt/${QT_VERSION}/gcc_64/doc /opt/qt/${QT_VERSION}/gcc_64/examples
WORKDIR /src
COPY . /src
RUN test -f externals/protobuf/CMakeLists.txt \
        || (echo "externals/protobuf is empty: run git submodule update --init" && exit 1)
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DCMAKE_PREFIX_PATH=/opt/qt/${QT_VERSION}/gcc_64 \
    && cmake --build build --target shadnet \
    && strip build/shadnet

FROM ubuntu:24.04
ARG QT_VERSION
ENV DEBIAN_FRONTEND=noninteractive
# Qt's own libs come from the build stage; these are what they load from the system.
# libpq5: the QPSQL plugin (unused today). sqlite3: for manual inspection only.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tini sqlite3 libglib2.0-0t64 libssl3t64 libpq5 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --home-dir /data --shell /usr/sbin/nologin shadnet
COPY --from=build /opt/qt/${QT_VERSION}/gcc_64/lib/ /opt/qt/lib/
COPY --from=build /opt/qt/${QT_VERSION}/gcc_64/plugins/sqldrivers/ /opt/qt/plugins/sqldrivers/
COPY --from=build /opt/qt/${QT_VERSION}/gcc_64/plugins/tls/ /opt/qt/plugins/tls/
COPY --from=build /src/build/shadnet /opt/shadnet/shadnet
COPY worlds.cfg scoreboards.cfg /opt/shadnet/defaults/
COPY hosting/entrypoint.sh /opt/shadnet/entrypoint.sh
ENV LD_LIBRARY_PATH=/opt/qt/lib QT_PLUGIN_PATH=/opt/qt/plugins

# shadNet works in SHADNET_HOME (main.cpp): shadnet.cfg, db/, bloodborne_worlds/, score_data/.
ENV SHADNET_HOME=/data
VOLUME /data
USER shadnet
# 31313 tcp game protocol, 31314 udp link/STUN, 31315 tcp WebAPI, 20443 tcp game backend.
# 31320 tcp stats: keep private.
EXPOSE 31313/tcp 31314/udp 31315/tcp 20443/tcp
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/shadnet/entrypoint.sh"]

# --- Debian alternative (Qt 6.8.2 from apt, no aqtinstall; also untested) ---
# build:   FROM debian:trixie; apt-get install ca-certificates git cmake ninja-build g++
#          qt6-base-dev qt6-httpserver-dev qt6-websockets-dev libqt6sql6-sqlite
# runtime: FROM debian:trixie; apt-get install tini sqlite3 libqt6core6t64 libqt6network6t64
#          libqt6sql6t64 libqt6concurrent6t64 libqt6httpserver6 libqt6websockets6
#          libqt6sql6-sqlite   (drop the /opt/qt copies and LD_LIBRARY_PATH)
