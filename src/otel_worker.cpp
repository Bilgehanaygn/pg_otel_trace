#include "otel_worker.h"
#include "shared_ring_buffer.h"

// OpenTelemetry Includes
#include "opentelemetry/sdk/trace/simple_processor_factory.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/exporters/ostream/span_exporter_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/resource/semantic_conventions.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include <stdatomic.h>
#include "config.h"

extern "C" {
    #include "miscadmin.h"
    #include "utils/elog.h"
}

namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;

static void InitOpenTelemetry() {
    // 1. Read the Service Name from our GUC
    resource::ResourceAttributes attributes = {
        {resource::SemanticConventions::kServiceName, otel_service_name},
        {"process.pid", MyProcPid} 
    };
    auto resource = resource::Resource::Create(attributes);

    // 2. Configure the HTTP Exporter using the Endpoint GUC
    opentelemetry::exporter::otlp::OtlpHttpExporterOptions opts;
    opts.url = otel_collector_endpoint; 
    
    auto exporter = opentelemetry::exporter::otlp::OtlpHttpExporterFactory::Create(opts);
    
    // 3. Use a BatchProcessor instead of a SimpleProcessor!
    // In production, we don't send an HTTP request for every single query.
    // The BatchProcessor gathers hundreds of spans and sends them in one giant HTTP payload.
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(std::move(exporter), {});

    std::shared_ptr<trace_api::TracerProvider> provider =
        trace_sdk::TracerProviderFactory::Create(std::move(processor), resource);
    trace_api::Provider::SetTracerProvider(provider);
    
    elog(LOG, "pg_otel_trace: OTel HTTP Exporter initialized pointing to %s", otel_collector_endpoint);
}

extern "C" void otel_background_worker_main(Datum main_arg) {
    InitOpenTelemetry(); 
    auto tracer = trace_api::Provider::GetTracerProvider()->GetTracer("postgres-bgworker");

    elog(LOG, "pg_otel_trace: Background Worker started.");

    while (true) {
        pg_usleep(100000L); // Sleep 100ms

        // Safety check in case memory didn't allocate
        if (SharedRingBuffer == NULL) continue;

        uint64_t current_read = pg_atomic_read_u64(&SharedRingBuffer->read_index);
        uint64_t current_write = pg_atomic_read_u64(&SharedRingBuffer->write_index);

        while (current_read < current_write) {
            uint64_t index = current_read % RING_BUFFER_CAPACITY;
            RawTraceEvent* event = &SharedRingBuffer->events[index];

            auto span = tracer->StartSpan("ExecuteQuery");
            span->SetAttribute("db.system", "postgresql");
            span->SetAttribute("db.statement", event->query_text);
            span->End(); 

            current_read++;
            pg_atomic_write_u64(&SharedRingBuffer->read_index, current_read);
        }
    }
}

void RegisterOtelWorker(){
    BackgroundWorker worker;
    memset(&worker, 0, sizeof(worker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "OTel Trace Exporter");
    snprintf(worker.bgw_type, BGW_MAXLEN, "otel_exporter");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = 10;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_otel_trace");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "otel_background_worker_main");

    RegisterBackgroundWorker(&worker);
}