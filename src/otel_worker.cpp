#include "otel_worker.h"
#include "shared_ring_buffer.h"
#include "config.h"

#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider.h" // Required to explicitly call Shutdown()
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/resource/semantic_conventions.h"

extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "utils/elog.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h" 
#include "libpq/pqsignal.h" 
}

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace resource  = opentelemetry::sdk::resource;

static volatile sig_atomic_t got_sigterm = false;

extern "C" void otel_sigterm(SIGNAL_ARGS) {
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

// Global raw pointers. We manage lifecycle natively via Postgres hooks.
static trace_sdk::TracerProvider* sdk_provider_raw = nullptr;
static opentelemetry::nostd::shared_ptr<trace_api::Tracer>* bg_tracer_ptr = nullptr;
static trace_api::Tracer* bg_tracer = nullptr;

static void otel_worker_cleanup(int code, Datum arg) {
    elog(LOG, "pg_otel_trace: Safely shutting down OpenTelemetry threads...");
    if (sdk_provider_raw) {
        // Explicitly flush pending spans and kill the BatchSpanProcessor thread.
        // This guarantees the thread is dead before standard exit(0) is called.
        sdk_provider_raw->ForceFlush(std::chrono::milliseconds(1000));
        sdk_provider_raw->Shutdown();
        sdk_provider_raw = nullptr;
    }

    if (bg_tracer_ptr) {
        delete bg_tracer_ptr;
        bg_tracer_ptr = nullptr;
        bg_tracer = nullptr;
    }
}

void RegisterOtelWorker() {
    BackgroundWorker worker;
    memset(&worker, 0, sizeof(BackgroundWorker));

    // When should this worker start? (After the DB is ready to accept queries)
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    
    // Allow the worker to read our SharedRingBuffer
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    
    // If it crashes, wait 1 second and restart it.
    worker.bgw_restart_time = 1;
    
    // Where to find the main loop function
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_otel_trace");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "otel_background_worker_main");
    
    // What it looks like in the OS process list (e.g., using 'top' or 'htop')
    snprintf(worker.bgw_name, BGW_MAXLEN, "OTel Trace Background Worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "pg_otel_trace");
    
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    // Hand it over to Postgres
    RegisterBackgroundWorker(&worker);
}

static void InitOpenTelemetry() {
    resource::ResourceAttributes attributes = {
        {resource::SemanticConventions::kServiceName, otel_service_name},
        {"process.pid", MyProcPid} 
    };
    auto resource = resource::Resource::Create(attributes);

    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    opts.url = otel_collector_endpoint; 
    opts.timeout = std::chrono::milliseconds(2000);
    
    opentelemetry::sdk::trace::BatchSpanProcessorOptions bsp_opts;
    bsp_opts.schedule_delay_millis = std::chrono::milliseconds(1000); 
    
    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(exporter), bsp_opts);

    // Intentionally allocate the provider in a container we will leak.
    auto* provider_container = new std::unique_ptr<trace_api::TracerProvider>(
        trace_sdk::TracerProviderFactory::Create(std::move(processor), resource)
    );
    
    // Extract the raw SDK provider so we can invoke Shutdown() later
    sdk_provider_raw = static_cast<trace_sdk::TracerProvider*>(provider_container->get());
    
    // Extract the tracer
    auto tracer_shared = (*provider_container)->GetTracer("postgres-bgworker");
    bg_tracer_ptr = new opentelemetry::nostd::shared_ptr<trace_api::Tracer>(tracer_shared);
    bg_tracer = bg_tracer_ptr->get();
}

extern "C" PGDLLEXPORT void otel_background_worker_main(Datum main_arg) {
    pqsignal(SIGTERM, otel_sigterm);
    BackgroundWorkerUnblockSignals();

    // 2. Register the cleanup hook IMMEDIATELY.
    on_proc_exit(otel_worker_cleanup, 0);

    InitOpenTelemetry(); 

    elog(LOG, "pg_otel_trace: Background Worker started.");

    while (!got_sigterm) {
        
        int rc = WaitLatch(MyLatch,
                           WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                           100L, 
                           0);
        
        ResetLatch(MyLatch);

        // If this hits, proc_exit(1) is called, but our on_proc_exit hook will STILL fire!
        if (rc & WL_POSTMASTER_DEATH) proc_exit(1);
        if (SharedRingBuffer == NULL || bg_tracer == nullptr) continue;

        uint64_t current_read = pg_atomic_read_u64(&SharedRingBuffer->read_index);
        uint64_t current_write = pg_atomic_read_u64(&SharedRingBuffer->write_index);

        while (current_read < current_write) {
            uint64_t index = current_read % RING_BUFFER_CAPACITY;
            RawTraceEvent* event = &SharedRingBuffer->events[index];

            std::chrono::nanoseconds start_time(event->start_time_unix_ns);
            std::chrono::system_clock::time_point span_start(start_time);
            std::chrono::steady_clock::time_point steady_baseline(std::chrono::nanoseconds(0));
            
            opentelemetry::trace::StartSpanOptions parent_opts;
            parent_opts.start_system_time = span_start;
            parent_opts.start_steady_time = steady_baseline;

            auto parent_span = bg_tracer->StartSpan("ExecuteQuery", parent_opts);
            parent_span->SetAttribute("db.system", "postgresql");
            parent_span->SetAttribute("db.statement", event->query_text);
            
            opentelemetry::trace::EndSpanOptions parent_end_opts;
            parent_end_opts.end_steady_time = steady_baseline + std::chrono::nanoseconds(event->query_duration_ns);
            parent_span->End(parent_end_opts);

            opentelemetry::trace::StartSpanOptions child_opts;
            child_opts.parent = parent_span->GetContext(); 
            child_opts.start_system_time = span_start;     
            child_opts.start_steady_time = steady_baseline;

            for (int i = 0; i < event->node_count; i++) {
                PlanNodeStat* stat = &event->nodes[i];

                auto child_span = bg_tracer->StartSpan(stat->node_type, child_opts);
                child_span->SetAttribute("db.rows_produced", stat->rows_produced);
                child_span->SetAttribute("db.node_id", stat->node_id);
                child_span->SetAttribute("db.parent_node_id", stat->parent_id);

                opentelemetry::trace::EndSpanOptions child_end_opts;
                child_end_opts.end_steady_time = steady_baseline + std::chrono::nanoseconds(stat->duration_ns);
                child_span->End(child_end_opts);
            }

            current_read++;
            pg_atomic_write_u64(&SharedRingBuffer->read_index, current_read);
        }
    }
    
    // Normal termination path. proc_exit(0) will naturally trigger otel_worker_cleanup.
    elog(LOG, "pg_otel_trace: Background Worker cleanly exiting main loop.");
    proc_exit(0); 
}