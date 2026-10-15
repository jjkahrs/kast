# --- deps: vcpkg at the pinned baseline; this layer only rebuilds when vcpkg.json changes ---
FROM ubuntu:24.04 AS deps
ARG VCPKG_COMMIT=ed8445dd2a9ba44e0e3ace4e952b2e33130082d9
ENV DEBIAN_FRONTEND=noninteractive VCPKG_ROOT=/opt/vcpkg VCPKG_FORCE_SYSTEM_BINARIES=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git curl ca-certificates zip unzip tar pkg-config ninja-build cmake \
      autoconf automake libtool bison flex perl python3 linux-libc-dev libtirpc-dev \
    && rm -rf /var/lib/apt/lists/*
# Full clone: manifest versioning reads port trees from git history.
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT && cd $VCPKG_ROOT \
    && git checkout $VCPKG_COMMIT && ./bootstrap-vcpkg.sh -disableMetrics
WORKDIR /src
COPY vcpkg.json ./
RUN $VCPKG_ROOT/vcpkg install --triplet x64-linux-release --host-triplet x64-linux-release \
      --x-install-root=/opt/vcpkg_installed --clean-after-build

# --- build: compile with warnings-as-errors, then run the self-check ---
FROM deps AS build
COPY . .
RUN cmake --preset linux-release -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF \
    && cmake --build --preset linux-release \
    && ctest --preset linux-release

# --- runtime: binaries + migrations, non-root ---
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 world
WORKDIR /app
COPY --from=build /src/build/linux-release/kast_world /src/build/linux-release/kast_bot ./
COPY migrations ./migrations
# Dev TLS cert lands here when none is configured; mount a volume to keep it.
RUN mkdir -p /app/certs && chown world:world /app/certs
USER world
EXPOSE 7777
CMD ["./kast_world"]
