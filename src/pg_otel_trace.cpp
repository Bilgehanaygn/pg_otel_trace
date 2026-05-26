extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/elog.h"
}

// Include our custom module headers
#include "config.h"
#include "shared_ring_buffer.h"
#include "otel_worker.h"
#include "executor_hooks.h"

extern "C" {
    PG_MODULE_MAGIC;
    void _PG_init(void);
    void _PG_fini(void);
}

void _PG_init(void) {
    elog(LOG, "pg_otel_trace: Initializing extension...");

    // 1. Define GUCs FIRST so they are available immediately
    DefineExtensionGUCs();

    RequestSharedMemory();
    RegisterOtelWorker();
    InstallExecutorHooks();
    
    elog(LOG, "pg_otel_trace: Initialization complete.");
}

void _PG_fini(void) {
    // Clean up our hooks so Postgres can unload safely
    UninstallExecutorHooks();
    elog(LOG, "pg_otel_trace: Extension unloaded.");
}