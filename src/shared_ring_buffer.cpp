#include "shared_ring_buffer.h"

extern "C" {
    #include "storage/shmem.h"
    #include "storage/ipc.h"
}

TraceRingBuffer *SharedRingBuffer = NULL;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void otel_shmem_startup(void) {
    if(prev_shmem_startup_hook) prev_shmem_startup_hook();

    bool found;
    SharedRingBuffer = (TraceRingBuffer *) ShmemInitStruct(
        "OTelTraceRingBuffer", sizeof(TraceRingBuffer), &found
    );

    if(!found) {
        pg_atomic_init_u64(&SharedRingBuffer->write_index, 0);
        pg_atomic_init_u64(&SharedRingBuffer->read_index, 0);
    }
}

void ReqestSharedMemory(){
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = otel_shmem_startup;
}
