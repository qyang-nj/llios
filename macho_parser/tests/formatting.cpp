#include <gtest/gtest.h>
#include "utils/utils.h"

TEST(FormatSize, ALL) {
    EXPECT_EQ(formatSize(0), "0B");
    EXPECT_EQ(formatSize(128), "128B");
    EXPECT_EQ(formatSize(1023), "1023B");
    EXPECT_EQ(formatSize(1024), "1.00KB");
    EXPECT_EQ(formatSize(1024 + 102), "1.10KB");
    EXPECT_EQ(formatSize(1024 * 1024), "1.00MB");
    EXPECT_EQ(formatSize(1024 * 1024 + 10), "1.00MB");
    EXPECT_EQ(formatSize(1024 * 1024 + 1024 * 10), "1.01MB");
    EXPECT_EQ(formatSize(1024 * 1024 * 1024), "1.00GB");
}

TEST(FormatBufferToHex, ALL) {
    EXPECT_EQ(formatBufferToHex((uint8_t *)"", 0), "");
    EXPECT_EQ(formatBufferToHex((uint8_t *)"\x1a", 1), "1a");
    EXPECT_EQ(formatBufferToHex((uint8_t *)"\x01\x02\x0a\x0f\xff", 5), "01020a0fff");
}


TEST(FormatStringLiteral, ALL) {
    EXPECT_EQ(formatStringLiteral(""), "");
    EXPECT_EQ(formatStringLiteral("\n"), "\\n");
    EXPECT_EQ(formatStringLiteral("\r"), "\\r");
    EXPECT_EQ(formatStringLiteral("abc\nxyz"), "abc\\nxyz");
    EXPECT_EQ(formatStringLiteral("\\"), "\\\\");
}
