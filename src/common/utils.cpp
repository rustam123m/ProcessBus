#include "utils.hpp"

#include <malloc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <iostream>

void init_linuxrt()
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        std::cerr << "Can't mlockall memory!\n";
    }
    mallopt(M_MMAP_MAX, 0);
    mallopt(M_TRIM_THRESHOLD, -1);

    /* Latency trick
     * if the file /dev/cpu_dma_latency exists,
     * open it and write a zero into it. This will tell
     * the power management system not to transition to
     * a high cstate (in fact, the system acts like idle=poll)
     * When the fd to /dev/cpu_dma_latency is closed, the behavior
     * goes back to the system default.
     *
     * Documentation/power/pm_qos_interface.txt
     */
    struct stat dmafile = {};
    if (stat("/dev/cpu_dma_latency", &dmafile) == 0) {
        static int s_latency_fd = 0;

        s_latency_fd = open("/dev/cpu_dma_latency", O_RDWR);
        if (s_latency_fd != -1) {
            uint32_t value = 0;
            int ret = write(s_latency_fd, &value, sizeof(value));
            if (ret == 0) {
                close(s_latency_fd);
            }
        }
    }
}

void set_thread_name(const std::string &name)
{
    pthread_setname_np(pthread_self(), name.c_str());
}

void set_thread_priority(int priority)
{
    sched_param param = {
        .sched_priority = priority
    };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}

void pin_thread_to_cpu(int cpu, int priority)
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

int create_signalfd()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    int fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Can't create Signal FD\n";
        abort();
    }
    return fd;
}

int create_timerfd(int periodSec)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Can't create Timer FD\n";
        abort();
    }

    itimerspec its = {};
    its.it_interval.tv_sec = periodSec;
    its.it_value.tv_sec = periodSec;
    timerfd_settime(fd, 0, &its, nullptr);
    return fd;
}

void display_packet_as_array(const uint8_t *packet, size_t packetSize)
{
    printf("const uint8_t packet[%zu] = {", packetSize);
    for (size_t i = 0; i < packetSize; ++i) {
        if (i % 16 == 0) {
            printf("\n    ");
        }
        printf("0x%02X", packet[i]);
        if (i < packetSize - 1) {
            printf(", ");
        }
    }
    printf("\n};\n");
}

