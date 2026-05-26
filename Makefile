MODULE_big = pg_otel_trace

OBJS = src/pg_otel_trace.o src/shared_ring_buffer.o src/executor_hooks.o src/otel_worker.o src/config.o
SHLIB_LINK += -lopentelemetry_trace -lopentelemetry_resources -lopentelemetry_exporter_otlp_http -lopentelemetry_otlp_http_client -lopentelemetry_common -lcurl -pthread


PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Custom compilation rule for C++ files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -std=c++14 -fPIC -c -o $@ $<

