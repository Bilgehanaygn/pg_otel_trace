#pragma once

#include <cstdint>

extern "C" {
#include "port/atomics.h"
}

#define MAX_QUERY_TEXT_LEN 1024
#define RING_BUFFER_CAPACITY 10000

struct RawTraceEvent {
    uint64_t trace_id;
    uint64_t duration_ns;
    char query_text[MAX_QUERY_TEXT_LEN];
};

//approximately 10mbs 
struct TraceRingBuffer {
    pg_atomic_uint64 write_index;
    pg_atomic_uint64 read_index;
    RawTraceEvent events[RING_BUFFER_CAPACITY];
}

extern TracingRingBuffer *SharedRingBuffer;

void RequestSharedMemory();