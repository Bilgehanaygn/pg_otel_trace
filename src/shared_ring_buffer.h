#pragma once

#include <cstdint>
#include <chrono>

extern "C" {
#include "postgres.h"
#include "port/atomics.h"
}

#define MAX_QUERY_TEXT_LEN 1024
#define MAX_PLAN_NODES 64
#define RING_BUFFER_CAPACITY 10000


struct PlanNodeStat {
    int node_id;
    int parent_id;
    char node_type[32];
    uint64_t duration_ns;
    uint64_t rows_produced;
};

struct RawTraceEvent {
    pg_atomic_uint32 ready; // 0 = empty/writing, 1 = payload ready for worker
    uint64_t trace_id;
    uint64_t start_time_unix_ns;
    uint64_t query_duration_ns;
    char query_text[MAX_QUERY_TEXT_LEN];

    int node_count;
    PlanNodeStat nodes[MAX_PLAN_NODES];
};

//approximately 10mbs 
struct TraceRingBuffer {
    pg_atomic_uint64 write_index;
    pg_atomic_uint64 read_index;
    pg_atomic_uint64 dropped_events; // Track buffer overflow occurences
    RawTraceEvent events[RING_BUFFER_CAPACITY];
};

extern TraceRingBuffer *SharedRingBuffer;

void RequestSharedMemory();