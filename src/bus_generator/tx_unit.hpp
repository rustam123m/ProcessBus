#pragma once

#include <cstdint>
#include <vector>

template< typename T >
struct TxBlock
{
    std::vector< T > packets;
};

template< typename T >
struct TxUnit
{
    typedef TxBlock< T > BlockType;

    uint64_t offsetUS = 0; // Timestamp

    std::vector< BlockType >  blocks;
};


/*
 * Burst mode needs a flat descriptor sequence, not a schedule: every packet of
 * the second goes out at the second boundary, so there is nothing to stamp with
 * an offset. Chunking is left to the sender, which walks the vector in bursts of
 * its own size - the stream count therefore has no say in how packets are
 * batched onto the wire.
 */
template< typename T >
std::vector< T > make_burst_plan(unsigned streams, unsigned reps)
{
    std::vector< T > plan;
    plan.reserve(static_cast< size_t >(streams) * reps);
    for (unsigned r=0;r<reps;++r) {
        for (unsigned s=0;s<streams;++s) {
            T desc;
            desc.idx = s;
            plan.push_back(desc);
        }
    }
    return plan;
}
