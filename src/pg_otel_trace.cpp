extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "miscadmin.h"
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
    // 1. Ensure we are loaded via shared_preload_libraries, not dynamically
    if (!process_shared_preload_libraries_in_progress) {
        elog(WARNING, "pg_otel_trace must be loaded via shared_preload_libraries");
        return;
    }

    // 2. Safety Check: Do not load in single-user or bootstrap modes (used by initdb)
    if (!IsPostmasterEnvironment || IsBootstrapProcessingMode()) {
        elog(LOG, "pg_otel_trace: Bootstrap/Single-user mode detected. Skipping extension load.");
        return;
    }

    elog(LOG, "pg_otel_trace: Initializing extension...");

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