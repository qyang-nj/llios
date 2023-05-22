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
