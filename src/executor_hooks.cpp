#include "executor_hooks.h"
#include "shared_ring_buffer.h"
#include <cstring>
#include <exception>

extern "C" {
#include "postgres.h"
#include "executor/executor.h"
#include "utils/elog.h"
}

// Helper function to keep our ring buffer logic clean
static void WriteTrace(QueryDesc *queryDesc, bool is_error) {
    if (SharedRingBuffer == NULL) return;

    uint64_t current_write = pg_atomic_read_u64(&SharedRingBuffer->write_index);
    uint64_t current_read = pg_atomic_read_u64(&SharedRingBuffer->read_index);

    if (current_write - current_read < RING_BUFFER_CAPACITY) {
        uint64_t index = current_write % RING_BUFFER_CAPACITY;
        RawTraceEvent* event = &SharedRingBuffer->events[index];
        
        event->trace_id = 12345; 
        event->duration_ns = 50000; // In reality, calculate end_time - start_time
        
        if (queryDesc->sourceText) {
            // If it's an error, we prepend "[FAILED] " to the query text
            if (is_error) {
                snprintf(event->query_text, MAX_QUERY_TEXT_LEN, "[FAILED] %s", queryDesc->sourceText);
            } else {
                strncpy(event->query_text, queryDesc->sourceText, MAX_QUERY_TEXT_LEN - 1);
            }
            event->query_text[MAX_QUERY_TEXT_LEN - 1] = '\0';
        }

        pg_atomic_write_u64(&SharedRingBuffer->write_index, current_write + 1);
    }
}

// ... (keep previous ExecutorStart hook) ...

// We must declare hook variables to save state
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static void profile_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once) {
    // We wrap the core execution in Postgres's exception handler
    PG_TRY();
    {
        // 1. Defend Postgres from C++: Catch any rogue C++ exceptions
        try {
            if (prev_ExecutorRun) prev_ExecutorRun(queryDesc, direction, count, execute_once);
            else standard_ExecutorRun(queryDesc, direction, count, execute_once);
        } 
        catch (const std::exception& e) {
            // Convert C++ exception to a Postgres C error!
            // This safely triggers a longjmp without crashing the whole server.
            elog(ERROR, "pg_otel_trace caught C++ exception: %s", e.what());
        }
        catch (...) {
            elog(ERROR, "pg_otel_trace caught unknown C++ exception");
        }
    }
    PG_CATCH();
    {
        // 2. Defend our Traces from Postgres: A query was aborted (e.g., Ctrl+C)!
        // ExecutorEnd will NOT be called. We must write our trace NOW.
        WriteTrace(queryDesc, true);
        
        // Pass the error back up the chain to Postgres so it can rollback the transaction
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static void profile_ExecutorEnd(QueryDesc *queryDesc) {
    // If we reach here, the query succeeded normally.
    WriteTrace(queryDesc, false);

    if (prev_ExecutorEnd) prev_ExecutorEnd(queryDesc);
    else standard_ExecutorEnd(queryDesc);
}

// Update the install function to include ExecutorRun
void InstallExecutorHooks() {
    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = profile_ExecutorRun;

    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = profile_ExecutorEnd;
}

void UninstallExecutorHooks() {
    ExecutorRun_hook = prev_ExecutorRun;
    ExecutorEnd_hook = prev_ExecutorEnd;
}