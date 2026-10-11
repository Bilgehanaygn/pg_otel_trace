# Ring buffer stress tests

This folder contains standalone concurrency tests for the ring-buffer logic used by the extension.

## Current test

- `ring_buffer_stress.cpp` is a standalone C++ stress test that simulates:
  - multiple producer threads writing into the ring buffer
  - a consumer thread reading batches of records
  - validation that payload data is never partially written or corrupted

## Build and run

```bash
cd /Users/hobby/pg-extension/pg_otel_trace
g++ -std=c++17 -O2 -pthread pg_regress_tests/ring_buffer_stress.cpp -o /tmp/ring_buffer_stress
/tmp/ring_buffer_stress
```

## Note

This is not a PostgreSQL `pg_regress` SQL file. `pg_regress` expects SQL scripts and expected-output files, but this concurrency stress test is valuable as a standalone validation before you wire the same behavior into PostgreSQL extension-level integration tests.
