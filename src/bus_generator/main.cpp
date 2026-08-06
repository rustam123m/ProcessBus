#include "gen_application.hpp"

#include <rte_eal.h>
#include <rte_ethdev.h>

#include <poll.h>

#include <cstdlib>
#include <format>
#include <thread>
#include <iostream>

volatile bool g_doWork = true;

static void* auxiliary_thread(GenApplication::ptr app)
{
    set_thread_name("aux_thread");
    pin_thread_to_cpu(0, 1);

    const unsigned TIMER_PERIOD_SEC = 3;
    int signalFD = create_signalfd();
    int timerFD = create_timerfd(TIMER_PERIOD_SEC);

    pollfd fds[2] = {
        { .fd = signalFD, .events= POLLIN, .revents = 0 },
        { .fd = timerFD,  .events= POLLIN, .revents = 0 }
    };

    while (g_doWork) {
        int res = poll(fds, sizeof(fds)/sizeof(fds[0]), 250);
        if (res < 0) {
            continue;
        }

        if (fds[0].revents & POLLIN) {
            signalfd_siginfo si;
            read(signalFD, &si, sizeof(si));

            std::cout << std::format("Received signal {0}, exiting", si.ssi_signo)
                      << std::endl;
            g_doWork = false;
        }

        if (fds[1].revents & POLLIN) {
            uint64_t expirations = 0;
            read(timerFD, &expirations, sizeof(expirations));

            app->DisplayStatistic(TIMER_PERIOD_SEC);
            if (app->IsTimeExpired()) {
                std::cout << "Run time limit reached, exiting" << std::endl;
                g_doWork = false;
            }
        }
    }

    close(signalFD);
    close(timerFD);
    return nullptr;
}

int main(int argc, char *argv[])
{
    // Block signals in all threads
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    // Init DPDK
    int retval = rte_eal_init(argc, argv);
    if (retval < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }
    // Skip DPDK's options
    argc -= retval;
    argv += retval;

    if (rte_eth_dev_count_avail() == 0) {
        rte_exit(EXIT_FAILURE, "No available ports. Check port binding.\n");
    }
    if (rte_get_main_lcore() == 0) {
        rte_exit(EXIT_FAILURE, "You can't use core 0 to generate/process BUSes!\n");
    }

    // RT
    init_linuxrt();

    // Thread identity, CPU core by DPDK's command
    set_thread_name("main");

    // Start auxiliary thread: signals & statistics
    std::thread auxThread;

    // Packet generator
    GenApplication::ptr app;

    try {
        app = std::make_shared< GenApplication >(argc, argv);

        auxThread = std::thread(auxiliary_thread, app);

        app->Run(g_doWork);
    } catch (const std::exception &exp) {
        std::cerr << "Exception: " << exp.what() << std::endl;
        g_doWork = false;
    }
    if (auxThread.joinable()) {
        auxThread.join();
    }

    rte_eal_cleanup();
    return 0;
}

