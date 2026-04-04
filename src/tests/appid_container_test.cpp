#include <gtest/gtest.h>

#include "bus_processor/appid_container.hpp"

TEST(AppIdContainer, BasicUsage)
{
    struct Key
    {
        uint16_t appid = 0;
        std::string goid;
    };
    struct Value
    {
        using ptr = std::shared_ptr< Value >;

        unsigned value = 0;
    };

    Key k[5] = { { .appid = 1, .goid = "GOID1" },
                 { .appid = 2, .goid = "GOID2" },
                 { .appid = 3, .goid = "GOID3" },
                 { .appid = 4, .goid = "GOID4" },
                 { .appid = 5, .goid = "GOID5" } };
    Value::ptr v[5] = { std::make_shared< Value >(),
                        std::make_shared< Value >(),
                        std::make_shared< Value >(),
                        std::make_shared< Value >(),
                        std::make_shared< Value >() };

    AppIdContainer< Key, Value::ptr > map;
    for (int i=0;i<5;++i) {
        map.insert(k[i], v[i]);
    }

    for (int i=0;i<5;++i) {
        ASSERT_EQ(map[k[i]], v[i]);
    }
}

TEST(AppIdContainer, MaxAppId)
{
    struct Key
    {
        uint16_t appid = 0;
        bool operator==(const Key &o) const { return appid == o.appid; }
    };
    using Value = int;

    AppIdContainer< Key, Value > map;

    Key maxKey{.appid = 65535};
    map.insert(maxKey, 42);

    auto it = map.find(maxKey);
    ASSERT_NE(it, map.end());
    ASSERT_EQ(it->second, 42);

    Key zeroKey{.appid = 0};
    map.insert(zeroKey, 99);

    auto it0 = map.find(zeroKey);
    ASSERT_NE(it0, map.end());
    ASSERT_EQ(it0->second, 99);
}

