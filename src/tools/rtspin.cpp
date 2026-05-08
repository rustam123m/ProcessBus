#include <iostream>
#include <sched.h>
#include <unistd.h>
#include <getopt.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static void pin_thread_to_cpu(int cpu, int priority)
{
    pthread_t thread = pthread_self();

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

    sched_param param = {
        .sched_priority = priority
    };
    pthread_setschedparam(thread, SCHED_FIFO, &param);
}

static inline uint64_t rdtsc()
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned hi, lo;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
#   error "rdtsc(): unsupported architecture"
#endif
}

int main(int argc, char* argv[])
{
    int cpu_core = 0;
    int prio = 99;
    bool do_yield = false;

    static struct option long_options[] = {
        {"cpu", required_argument, nullptr, 'c'},
        {"priority", required_argument, nullptr, 'p'},
        {"yield", no_argument, nullptr, 'y'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:y", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                cpu_core = atoi(optarg);
                break;
            case 'p':
                prio = atoi(optarg);
                break;
            case 'y':
                do_yield = true;
                break;
            default:
                std::cerr << "Usage: " << argv[0]
                          << " [--cpu <core>] [--priority <prio>] [--yield]\n";
                exit(EXIT_FAILURE);
        }
    }

    pin_thread_to_cpu(cpu_core, prio);

    std::cout << "Running on CPU " << cpu_core << " with priority " << prio
              << (do_yield ? " [with yield]" : " [without yield]") << std::endl;

    uint64_t tsc_last = rdtsc();
    const uint64_t freq = 2'000'000'000ULL;  // assume 2GHz
    const uint64_t threshold = freq * 1;     // 1 second in TSC ticks

    while (1) {
        uint64_t now = rdtsc();
        if (do_yield && (now - tsc_last) > threshold) {
            sched_yield();
            tsc_last = now;
        }
    }
}

