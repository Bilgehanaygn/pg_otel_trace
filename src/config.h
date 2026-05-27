#pragma once

// Global variables to hold the user's configuration
// In C++, char* is a pointer to a string. Postgres will manage this memory.
extern char* otel_collector_endpoint;
extern char* otel_service_name;
extern char* otel_db_name;

// Signature to register the variables with Postgres
void DefineExtensionGUCs();