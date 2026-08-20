#include "bus_generator/tx_unit.hpp"
#include "bus_generator/goose_traffic_gen.hpp"

#include <gtest/gtest.h>

#include <map>

/*
 * The burst plan is what --burst sends at the second boundary. It is flat by
 * design: chunking onto the wire belongs to the sender, so the stream count must
 * not decide how packets are batched.
 */
TEST(BurstPlan, HoldsEveryPublicationOfTheSecond)
{
    const unsigned streams = 32, reps = 128;
    auto plan = make_burst_plan< GoosePacketDesc >(streams, reps);

    EXPECT_EQ(plan.size(), streams * reps);
}

TEST(BurstPlan, RepeatsEachStreamOncePerRound)
{
    const unsigned streams = 32, reps = 128;
    auto plan = make_burst_plan< GoosePacketDesc >(streams, reps);

    std::map< unsigned, unsigned > seen;
    for (const auto &desc : plan) {
        ++seen[desc.idx];
    }

    ASSERT_EQ(seen.size(), streams);
    for (const auto &[idx, count] : seen) {
        EXPECT_EQ(count, reps) << "stream " << idx;
    }
}

// Each round is one publication from every stream, in order. With 32 streams a
// round is exactly one 32-packet TX chunk, so a dropped chunk spreads evenly
// over the streams instead of gutting one of them.
TEST(BurstPlan, WalksStreamsInOrderWithinEachRound)
{
    const unsigned streams = 32, reps = 4;
    auto plan = make_burst_plan< GoosePacketDesc >(streams, reps);

    for (unsigned i=0;i<plan.size();++i) {
        EXPECT_EQ(plan[i].idx, i % streams) << "at " << i;
    }
}

// A single stream still yields a flat plan the sender can chunk into 32s; the
// naive "one block per publication" shape would have made it 4096 sends of one.
TEST(BurstPlan, SingleStreamStaysFlat)
{
    auto plan = make_burst_plan< GoosePacketDesc >(1, 4096);

    ASSERT_EQ(plan.size(), 4096u);
    for (const auto &desc : plan) {
        EXPECT_EQ(desc.idx, 0u);
    }
}

TEST(BurstPlan, EmptyWhenNothingToSend)
{
    EXPECT_TRUE(make_burst_plan< GoosePacketDesc >(0, 4096).empty());
    EXPECT_TRUE(make_burst_plan< GoosePacketDesc >(32, 0).empty());
}
