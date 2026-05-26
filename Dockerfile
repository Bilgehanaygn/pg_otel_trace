# Use the official Postgres 16 image as a base
FROM postgres:16-bookworm

# Install build tools, C++ compiler, Postgres dev headers, and OTel dependencies (curl, cmake)
RUN apt-get update && apt-get install -y \
    build-essential \
    g++ \
    make \
    cmake \
    git \
    libcurl4-openssl-dev \
    postgresql-server-dev-16 \
    && rm -rf /var/lib/apt/lists/*

# Build OpenTelemetry C++ (ostream exporter only; OTLP pulls in broken benchmark targets on main)
WORKDIR /tmp
RUN git clone --depth 1 --branch v1.21.0 https://github.com/open-telemetry/opentelemetry-cpp.git && \
    cd opentelemetry-cpp && \
    mkdir build && cd build && \
    cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DBUILD_TESTING=OFF \
      -DWITH_BENCHMARK=OFF \
      -DWITH_METRICS_PREVIEW=OFF \
      -DWITH_LOGS_PREVIEW=OFF \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DWITH_OTLP_HTTP=OFF \
      -DWITH_OTLP_GRPC=OFF && \
    cmake --build . --target install -j "$(nproc)" && \
    cd /tmp && rm -rf opentelemetry-cpp

WORKDIR /workspace
COPY Makefile pg_otel_trace.control ./
COPY src/ src/
RUN make && make install