#include "config.h"

extern "C" {
#include "postgres.h"
#include "utils/guc.h"
}

// 1. Initialize our global variables with default values
char* otel_collector_endpoint = NULL;
char* otel_service_name = NULL;

void DefineExtensionGUCs() {
    // 2. Define the Collector Endpoint Variable
    DefineCustomStringVariable(
        "pg_otel.collector_endpoint",    // The name in postgresql.conf
        "URL of the OpenTelemetry OTLP HTTP Collector", // Short description
        NULL,                            // Long description
        &otel_collector_endpoint,        // The C++ variable it writes to
        "http://localhost:4318/v1/traces", // Default value
        PGC_SIGHUP,                      // Can be reloaded without restarting the DB
        0,                               // Flags
        NULL, NULL, NULL                 // Validation hooks (not needed here)
    );

    // 3. Define the Service Name Variable
    DefineCustomStringVariable(
        "pg_otel.service_name",
        "The service.name attribute attached to spans",
        NULL,
        &otel_service_name,
        "postgres-production-db",
        PGC_SIGHUP,
        0,
        NULL, NULL, NULL
    );
}