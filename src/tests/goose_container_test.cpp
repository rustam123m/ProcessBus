#include "common/goose_container.hpp"

#include <gtest/gtest.h>

class GooseSequenceTest : public ::testing::Test
{
protected:
    GooseSource src;
    GoosePassport pass;
    GooseState state;

    void SetUp() override {
        src.SetMAC(MAC("01:0C:CD:04:00:01"))
           .SetAppID(1)
           .SetGOID("GOID")
           .SetGOCBRef("GOCB")
           .SetDataSetRef("DS")
           .SetCRev(1)
           .SetNumEntries(1);
        pass = src.GetPassport();
    }

    void ProcessWithStNum(uint32_t stNum) {
        state.stNum = stNum;
        state.sqNum = 0;
        src.ProcessState(pass, state);
    }
};

TEST_F(GooseSequenceTest, NormalIncrement)
{
    ProcessWithStNum(1);
    ProcessWithStNum(2);
    ProcessWithStNum(3);
    ASSERT_EQ(src.GetErrSeqNum(), 0);
}

TEST_F(GooseSequenceTest, SkipAhead)
{
    ProcessWithStNum(1);
    ProcessWithStNum(3); // skipped 2
    ASSERT_EQ(src.GetErrSeqNum(), 1);
}

TEST_F(GooseSequenceTest, BackwardJump)
{
    ProcessWithStNum(1);
    ProcessWithStNum(2);
    ProcessWithStNum(3);
    ProcessWithStNum(1); // backward
    ASSERT_EQ(src.GetErrSeqNum(), 1);
}

TEST_F(GooseSequenceTest, SameStNumRepeated)
{
    ProcessWithStNum(1);
    ProcessWithStNum(1); // retransmission
    ASSERT_EQ(src.GetErrSeqNum(), 0);
}

TEST_F(GooseSequenceTest, MultipleViolations)
{
    ProcessWithStNum(1);
    ProcessWithStNum(5);  // skip → err 1
    ProcessWithStNum(10); // skip → err 2
    ProcessWithStNum(2);  // backward → err 3
    ASSERT_EQ(src.GetErrSeqNum(), 3);
}
