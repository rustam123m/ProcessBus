#pragma once

enum ThreadPriorities
{
    DEF_WORKER_PRIORITY    = 90,
    DEF_PROCESS_PRIORITY   = 90,
    DEF_GENERATOR_PRIORITY = 90
};

enum ThreadCores
{
    DEF_BUS_RX_CPU = 1,
    DEF_BUS_TX_CPU = 2
};

constexpr unsigned DEF_GOOSE_ENTRIES = 16;

