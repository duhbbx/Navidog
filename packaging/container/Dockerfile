FROM debian:13-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends     build-essential cmake ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt LICENSE NOTICE ./
COPY apps/sql/ apps/sql/
COPY include/navidog/sql/ include/navidog/sql/
COPY src/sql/ src/sql/
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release     -DNAVIDOG_COMPILER_ONLY=ON -DBUILD_TESTING=OFF     "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" &&     cmake --build /build --parallel 4 --target navidog-sql &&     /build/bin/navidog-sql --help

FROM debian:13-slim
LABEL org.opencontainers.image.source="https://github.com/duhbbx/Navidog"       org.opencontainers.image.description="Navidog command-line SQL parser, formatter and optimizer"       org.opencontainers.image.licenses="Apache-2.0"
COPY --from=build /build/bin/navidog-sql /usr/local/bin/navidog-sql
COPY LICENSE NOTICE /usr/share/doc/navidog/
WORKDIR /work
USER 65532:65532
ENTRYPOINT ["/usr/local/bin/navidog-sql"]
CMD ["--help"]
