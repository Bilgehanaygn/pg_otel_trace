#include "executor_hooks.h"
#include "shared_ring_buffer.h"
#include <cstring>
#include <exception>
#include <chrono>

extern "C" {
#include "postgres.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodes.h"
#include "utils/elog.h"
}

// Paste this helper function above ExtractNodeStats
static const char* GetNodeName(NodeTag tag) {
    switch (tag) {
        case T_Result: return "Result";
        case T_Append: return "Append";
        case T_MergeAppend: return "MergeAppend";
        case T_SeqScan: return "Seq Scan";
        case T_SampleScan: return "Sample Scan";
        case T_IndexScan: return "Index Scan";
        case T_IndexOnlyScan: return "Index Only Scan";
        case T_BitmapIndexScan: return "Bitmap Index Scan";
        case T_BitmapHeapScan: return "Bitmap Heap Scan";
        case T_TidScan: return "Tid Scan";
        case T_SubqueryScan: return "Subquery Scan";
        case T_FunctionScan: return "Function Scan";
        case T_ValuesScan: return "Values Scan";
        case T_CteScan: return "CTE Scan";
        case T_WorkTableScan: return "WorkTable Scan";
        case T_ForeignScan: return "Foreign Scan";
        case T_CustomScan: return "Custom Scan";
        case T_NestLoop: return "Nested Loop";
        case T_MergeJoin: return "Merge Join";
        case T_HashJoin: return "Hash Join";
        case T_Material: return "Materialize";
        case T_Memoize: return "Memoize";
        case T_Sort: return "Sort";
        case T_IncrementalSort: return "Incremental Sort";
        case T_Group: return "Group";
        case T_Agg: return "Aggregate";
        case T_WindowAgg: return "WindowAgg";
        case T_Unique: return "Unique";
        case T_Gather: return "Gather";
        case T_GatherMerge: return "Gather Merge";
        case T_Hash: return "Hash";
        case T_Limit: return "Limit";
        default: return "Unknown Node";
    }
}

// 1. Recursive Tree Extraction
static void ExtractNodeStats(PlanState *planstate, int parent_id, RawTraceEvent* event) {
    if (planstate == NULL || event->node_count >= MAX_PLAN_NODES) return;

    int current_id = event->node_count;
    PlanNodeStat* stat = &event->nodes[current_id];
    event->node_count++;

    stat->node_id = current_id;
    stat->parent_id = parent_id;
    
    strncpy(stat->node_type, GetNodeName(nodeTag(planstate->plan)), 31);
    stat->node_type[31] = '\0';

    if (planstate->instrument) {
        stat->duration_ns = (uint64_t)(planstate->instrument->total * 1000000000.0);
        stat->rows_produced = (uint64_t)planstate->instrument->ntuples;
    } else {
        stat->duration_ns = 0;
        stat->rows_produced = 0;
    }

    ExtractNodeStats(planstate->lefttree, current_id, event);
    ExtractNodeStats(planstate->righttree, current_id, event);

    if (IsA(planstate->plan, Append)) {
        AppendState *as = (AppendState *) planstate;
        for (int i = 0; i < as->as_nplans; i++) {
            ExtractNodeStats(as->appendplans[i], current_id, event);
        }
    }
}

// 2. The Unified Write Function (Handles both success and errors)
static void WriteTrace(QueryDesc *queryDesc, bool is_error) {
    if (SharedRingBuffer == NULL) return;

    uint64_t current_write = pg_atomic_read_u64(&SharedRingBuffer->write_index);
    uint64_t current_read = pg_atomic_read_u64(&SharedRingBuffer->read_index);

    if (current_write - current_read < RING_BUFFER_CAPACITY) {
        uint64_t index = current_write % RING_BUFFER_CAPACITY;
        RawTraceEvent* event = &SharedRingBuffer->events[index];
        
        auto now = std::chrono::system_clock::now();
        event->start_time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        
        if (queryDesc->sourceText) {
            if (is_error) {
                snprintf(event->query_text, MAX_QUERY_TEXT_LEN, "[FAILED] %s", queryDesc->sourceText);
            } else {
                strncpy(event->query_text, queryDesc->sourceText, MAX_QUERY_TEXT_LEN - 1);
            }
            event->query_text[MAX_QUERY_TEXT_LEN - 1] = '\0';
        }

        event->node_count = 0;

        if (queryDesc->planstate) {
            ExtractNodeStats(queryDesc->planstate, -1, event);
        }

        if (event->node_count > 0) {
            event->query_duration_ns = event->nodes[0].duration_ns;
        } else {
            event->query_duration_ns = 0;
        }

        pg_atomic_write_u64(&SharedRingBuffer->write_index, current_write + 1);
    }
}

// 3. The Hooks
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static void profile_ExecutorStart(QueryDesc *queryDesc, int eflags) {
    // Turn on native instrumentation
    queryDesc->instrument_options |= INSTRUMENT_TIMER | INSTRUMENT_ROWS;

    if (prev_ExecutorStart) prev_ExecutorStart(queryDesc, eflags);
    else standard_ExecutorStart(queryDesc, eflags);
}

static void profile_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once) {
    PG_TRY();
    {
        try {
            if (prev_ExecutorRun) prev_ExecutorRun(queryDesc, direction, count, execute_once);
            else standard_ExecutorRun(queryDesc, direction, count, execute_once);
        } 
        catch (const std::exception& e) {
            elog(ERROR, "pg_otel_trace caught C++ exception: %s", e.what());
        }
        catch (...) {
            elog(ERROR, "pg_otel_trace caught unknown C++ exception");
        }
    }
    PG_CATCH();
    {
        // Query crashed mid-flight! Write the trace tree we have so far.
        WriteTrace(queryDesc, true);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

static void profile_ExecutorEnd(QueryDesc *queryDesc) {
    // Query succeeded normally.
    WriteTrace(queryDesc, false);

    if (prev_ExecutorEnd) prev_ExecutorEnd(queryDesc);
    else standard_ExecutorEnd(queryDesc);
}

void InstallExecutorHooks() {
    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = profile_ExecutorStart;
    
    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = profile_ExecutorRun;

    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = profile_ExecutorEnd;
}

void UninstallExecutorHooks() {
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorRun_hook = prev_ExecutorRun;
    ExecutorEnd_hook = prev_ExecutorEnd;
}