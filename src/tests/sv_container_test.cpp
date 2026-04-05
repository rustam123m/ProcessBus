#include "common/sv_container.hpp"

#include <gtest/gtest.h>

class SVSequenceTest : public ::testing::Test
{
protected:
    SVStreamSource src;
    SVStreamPassport pass;
    SVStreamState state;

    void SetUpSV(uint16_t numASDU) {
        src.SetMAC(MAC("01:0C:CD:01:00:01"))
           .SetAppID(1)
           .SetSVID("SVID")
           .SetCRev(1)
           .SetNumASDU(numASDU);
        pass = src.GetPassport();
    }

    void ProcessWithSmpCnt(uint16_t smpCnt) {
        state.smpCnt = smpCnt;
        src.ProcessState(pass, state);
    }
};

TEST_F(SVSequenceTest, NormalIncrementSV80)
{
    SetUpSV(1);
    ProcessWithSmpCnt(0);
    ProcessWithSmpCnt(1);
    ProcessWithSmpCnt(2);
    ProcessWithSmpCnt(3);
    ASSERT_EQ(src.GetErrSeqNum(), 0);
}

TEST_F(SVSequenceTest, NormalIncrementSV256)
{
    SetUpSV(8);
    ProcessWithSmpCnt(0);
    ProcessWithSmpCnt(8);
    ProcessWithSmpCnt(16);
    ProcessWithSmpCnt(24);
    ASSERT_EQ(src.GetErrSeqNum(), 0);
}

TEST_F(SVSequenceTest, WrapToZeroSV80)
{
    SetUpSV(1);
    // Build valid sequence ending at 3999
    ProcessWithSmpCnt(0);
    for (uint16_t i = 1; i <= 3999; ++i) {
        ProcessWithSmpCnt(i);
    }
    ProcessWithSmpCnt(0); // wrap
    ASSERT_EQ(src.GetErrSeqNum(), 0);
}

TEST_F(SVSequenceTest, WrapToZeroSV256)
{
    SetUpSV(8);
    // Build valid sequence ending at 12792
    ProcessWithSmpCnt(0);
    for (uint16_t i = 8; i <= 12792; i += 8) {
        ProcessWithSmpCnt(i);
    }
    ProcessWithSmpCnt(0); // wrap
    ASSERT_EQ(src.GetErrSeqNum(), 0);
}

TEST_F(SVSequenceTest, WrongDelta)
{
    SetUpSV(1);
    ProcessWithSmpCnt(0);
    ProcessWithSmpCnt(5); // expected 1
    ASSERT_EQ(src.GetErrSeqNum(), 1);
}

TEST_F(SVSequenceTest, WrongDeltaSV256)
{
    SetUpSV(8);
    ProcessWithSmpCnt(0);
    ProcessWithSmpCnt(1); // expected 8
    ASSERT_EQ(src.GetErrSeqNum(), 1);
}
