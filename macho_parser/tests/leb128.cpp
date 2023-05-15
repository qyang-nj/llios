#include <gtest/gtest.h>
#include "utils/utils.h"

TEST(LEB128, OneByteULEB128) {
    uint8_t bytes[] = {0x02, 0x03, 0x04};
    uint64_t num;
    int size = readULEB128(bytes, &num);

    EXPECT_EQ(num, 2);
    EXPECT_EQ(size, 1);
}

TEST(LEB128, MultiBytesULEB128) {
    uint8_t bytes[] = {0xE5, 0x8E, 0x26};
    uint64_t num;
    int size = readULEB128(bytes, &num);

    EXPECT_EQ(num, 624485);
    EXPECT_EQ(size, 3);
}

TEST(LEB128, MultiBytesSLEB128) {
    uint8_t bytes[] = {0xC0, 0xBB, 0x78};
    int64_t num;
    int size = readSLEB128(bytes, &num);

    EXPECT_EQ(num, -123456);
    EXPECT_EQ(size, 3);
}
