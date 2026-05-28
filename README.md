# pg_otel_trace

`pg_otel_trace` is a PostgreSQL extension that traces query execution and exports each execution plan node's runtime as a separate OpenTelemetry span.

## Goal

The project builds a Postgres extension that hooks into the executor and records fine-grained timing information for individual plan nodes. The extension is intended for use with OpenTelemetry-compatible backends such as Jaeger.

## High-level flow

```text
Client SQL query
       │
       ▼
Postgres executor
       │
       ▼
Executor hooks in extension
       │
       ▼
Shared ring buffer (in-memory queue)
       │
       ▼
OTel worker thread reads buffer
       │
       ▼
Builds spans and exports via OTLP
       │
       ▼
Jaeger / OpenTelemetry backend
```

- `executor_hooks` captures per-plan-node execution timing and writes events to the ring buffer.
- `shared_ring_buffer` stores events safely for asynchronous export.
- `otel_worker` consumes buffer events, converts them to OpenTelemetry spans, and sends them out via OTLP.

## Quick start

1. Build the Docker image:

   ```bash
   docker compose build db
   ```

2. Start the stack:

   ```bash
   docker compose up
   ```

3. Open the Jaeger UI:

   - http://localhost:16686

4. Connect to Postgres on:

   - `localhost:5432`

## Notes

- The Docker image builds the extension from the repository using the provided `Dockerfile`.
- The `docker-compose.yml` service mounts the repository into the container and also mounts `scripts/01-enable-extension.sh` into `/docker-entrypoint-initdb.d/` so the extension is enabled when the database initializes.
- If you change source files, rebuild the image with:

   ```bash
   docker compose build --no-cache db
   ```

- Stop the stack with:

   ```bash
   docker compose down
   ```
