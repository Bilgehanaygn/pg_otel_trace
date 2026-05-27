MODULE_big = pg_otel_trace

OBJS = src/pg_otel_trace.o src/shared_ring_buffer.o src/executor_hooks.o src/otel_worker.o src/config.o

# OpenTelemetry C++ is installed to /usr/local by the Dockerfile
PG_CPPFLAGS += -I/usr/local/include
SHLIB_LINK += -L/usr/local/lib \
              -lopentelemetry_exporter_otlp_http \
              -lopentelemetry_exporter_otlp_http_client \
              -lopentelemetry_http_client_curl \
              -lopentelemetry_otlp_recordable \
              -lopentelemetry_proto \
              -lopentelemetry_trace \
              -lopentelemetry_logs \
              -lopentelemetry_resources \
              -lopentelemetry_common \
              -lprotobuf \
              -lcurl -pthread


PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Custom compilation rule for C++ files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -std=c++14 -fPIC -c -o $@ $<

