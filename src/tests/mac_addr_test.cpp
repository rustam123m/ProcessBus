#include "common/mac_addr.hpp"

#include <gtest/gtest.h>

TEST(MACAddress, ValidFromString)
{
    MAC mac("01:0C:CD:04:00:01");
    ASSERT_EQ(mac[0], 0x01);
    ASSERT_EQ(mac[1], 0x0C);
    ASSERT_EQ(mac[2], 0xCD);
    ASSERT_EQ(mac[3], 0x04);
    ASSERT_EQ(mac[4], 0x00);
    ASSERT_EQ(mac[5], 0x01);
}

TEST(MACAddress, ValidFromBytes)
{
    uint8_t bytes[] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    MAC mac(bytes);
    ASSERT_EQ(mac[0], 0xFF);
    ASSERT_EQ(mac[5], 0xAA);
}

TEST(MACAddress, InvalidLength)
{
    ASSERT_THROW(MAC(""), std::invalid_argument);
    ASSERT_THROW(MAC("01:02:03:04:05"), std::invalid_argument);
    ASSERT_THROW(MAC("01:02:03:04:05:06:07"), std::invalid_argument);
}

TEST(MACAddress, InvalidFormat)
{
    ASSERT_THROW(MAC("01.0C.CD.04.00.01"), std::invalid_argument);
    ASSERT_THROW(MAC("01-0C-CD-04-00-01"), std::invalid_argument);
    ASSERT_THROW(MAC("ZZZZZZZZZZZZZZZZZ"), std::invalid_argument);
}

TEST(MACAddress, Equality)
{
    MAC a("01:0C:CD:04:00:01");
    MAC b("01:0C:CD:04:00:01");
    MAC c("01:0C:CD:04:00:02");

    ASSERT_EQ(a, b);
    ASSERT_NE(a, c);
}

TEST(MACAddress, DefaultIsZero)
{
    MAC mac;
    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(mac[i], 0);
    }
}

TEST(MACAddress, ToString)
{
    MAC mac("AB:CD:EF:01:23:45");
    ASSERT_EQ(mac.toString(), "AB:CD:EF:01:23:45");
    ASSERT_EQ(mac.toString('-'), "AB-CD-EF-01-23-45");
}

TEST(MACAddress, ToU64)
{
    MAC mac("01:02:03:04:05:06");
    uint64_t val = mac.toU64();
    ASSERT_EQ(val, 0x010203040506ULL);
}
