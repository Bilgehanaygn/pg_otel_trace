#!/bin/bash
set -e

# This script runs against the temporary server during init.
# We append our custom settings directly to the main config file.
echo "shared_preload_libraries = 'pg_otel_trace'" >> "$PGDATA/postgresql.conf"
echo "pg_otel.collector_endpoint = 'http://jaeger:4318/v1/traces'" >> "$PGDATA/postgresql.conf"

echo "pg_otel_trace configuration successfully written to postgresql.conf"