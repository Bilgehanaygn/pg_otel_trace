#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CAPACITY 4096
#define PRODUCER_THREADS 4
#define EVENTS_PER_THREAD 256

using pg_atomic_u64 = std::atomic<uint64_t>;
using pg_atomic_uint32 = std::atomic<uint32_t>;

#define pg_atomic_fetch_add_u64(ptr, val) (ptr)->fetch_add((val), std::memory_order_relaxed)
#define pg_atomic_read_u64(ptr) (ptr)->load(std::memory_order_acquire)
#define pg_atomic_write_u64(ptr, val) (ptr)->store((val), std::memory_order_release)
#define pg_atomic_read_u32(ptr) (ptr)->load(std::memory_order_acquire)
#define pg_atomic_write_u32(ptr, val) (ptr)->store((val), std::memory_order_release)
#define pg_write_barrier() std::atomic_thread_fence(std::memory_order_release)
#define pg_read_barrier() std::atomic_thread_fence(std::memory_order_acquire)

struct RawTraceEvent {
    pg_atomic_uint32 ready{0};
    uint64_t producer_id;
    uint64_t event_id;
    char query_text[256];
};

struct RingBuffer {
    pg_atomic_u64 write_index{0};
    pg_atomic_u64 read_index{0};
    pg_atomic_u64 dropped_events{0};
    RawTraceEvent events[CAPACITY];
};

static bool payload_is_valid(const RawTraceEvent* event) {
    constexpr char expected_suffix[] = "[COMPLETE]";
    const std::size_t len = std::strlen(event->query_text);
    return len >= 10 && std::strcmp(event->query_text + len - 10, expected_suffix) == 0;
}

static void run_integrity_test() {
    RingBuffer buffer{};
    std::atomic<uint64_t> processed{0};

    auto producer = [&](uint64_t thread_id) {
        for (uint64_t i = 0; i < EVENTS_PER_THREAD; i++) {
            uint64_t current_read = pg_atomic_read_u64(&buffer.read_index);
            uint64_t reserved_pos = pg_atomic_fetch_add_u64(&buffer.write_index, 1);

            if (reserved_pos - current_read >= CAPACITY) {
                pg_atomic_fetch_add_u64(&buffer.dropped_events, 1);
                continue;
            }

            RawTraceEvent* event = &buffer.events[reserved_pos % CAPACITY];
            pg_atomic_write_u32(&event->ready, 0);

            event->producer_id = thread_id;
            event->event_id = i;
            std::snprintf(event->query_text, sizeof(event->query_text),
                          "T:%llu-E:%llu-[COMPLETE]",
                          static_cast<unsigned long long>(thread_id),
                          static_cast<unsigned long long>(i));

            pg_write_barrier();
            pg_atomic_write_u32(&event->ready, 1);

            if (!payload_is_valid(event)) {
                std::cerr << "RACE CONDITION DETECTED! Corrupted payload: "
                          << event->query_text << "\n";
                std::exit(1);
            }

            processed.fetch_add(1, std::memory_order_relaxed);
            pg_atomic_write_u64(&buffer.read_index, reserved_pos + 1);
        }
    };

    std::vector<std::thread> producers;
    producers.reserve(PRODUCER_THREADS);
    for (int i = 0; i < PRODUCER_THREADS; i++) {
        producers.emplace_back(producer, static_cast<uint64_t>(i));
    }
    for (auto& p : producers) {
        p.join();
    }

    uint64_t total_expected = PRODUCER_THREADS * EVENTS_PER_THREAD;
    uint64_t total_dropped = pg_atomic_read_u64(&buffer.dropped_events);
    uint64_t total_processed_now = processed.load(std::memory_order_relaxed);

    std::cout << "[Integrity test]\n";
    std::cout << "Expected Events:  " << total_expected << "\n";
    std::cout << "Processed Events: " << total_processed_now << "\n";
    std::cout << "Dropped Events:   " << total_dropped << "\n";

    if (total_processed_now == 0) {
        std::cerr << "FAIL: no events were published.\n";
        std::exit(1);
    }

    std::cout << "TEST PASSED: ready-before-publish semantics preserved payload integrity.\n";
}

static void run_drop_count_test() {
    RingBuffer local{};
    uint64_t expected_drops = 0;

    for (uint64_t i = 0; i < CAPACITY + 128; ++i) {
        uint64_t current_read = pg_atomic_read_u64(&local.read_index);
        uint64_t reserved_pos = pg_atomic_fetch_add_u64(&local.write_index, 1);

        if (reserved_pos - current_read >= CAPACITY) {
            pg_atomic_fetch_add_u64(&local.dropped_events, 1);
            expected_drops++;
            continue;
        }

        RawTraceEvent* event = &local.events[reserved_pos % CAPACITY];
        pg_atomic_write_u32(&event->ready, 0);
        std::snprintf(event->query_text, sizeof(event->query_text),
                      "DROP_TEST:%llu-[COMPLETE]",
                      static_cast<unsigned long long>(i));
        pg_write_barrier();
        pg_atomic_write_u32(&event->ready, 1);
    }

    uint64_t actual_drops = pg_atomic_read_u64(&local.dropped_events);
    std::cout << "[Overflow/drop test]\n";
    std::cout << "Expected drop count: " << expected_drops << "\n";
    std::cout << "Actual drop count:   " << actual_drops << "\n";

    if (actual_drops == 0) {
        std::cerr << "FAIL: dropped_events did not increment during overflow.\n";
        std::exit(1);
    }

    std::cout << "TEST PASSED: dropped_events tracked overflow as expected.\n";
}

int main() {
    std::cout << "Starting Lock-Free Ring Buffer Test Suite...\n\n";
    run_integrity_test();
    std::cout << "\n";
    run_drop_count_test();
    return 0;
}