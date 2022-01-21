// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "transmission.h"
#include "history.h"

#include "gtest/gtest.h"

TEST(History, recentHistory)
{
    auto h = tr_recentHistory{};

    h.add(10000, 1);
    EXPECT_EQ(0, h.count(12000, 1000));
    EXPECT_EQ(1, h.count(12000, 3000));
    EXPECT_EQ(1, h.count(12000, 5000));
    h.add(20000, 1);
    EXPECT_EQ(0, h.count(22000, 1000));
    EXPECT_EQ(1, h.count(22000, 3000));
    EXPECT_EQ(2, h.count(22000, 15000));
    EXPECT_EQ(2, h.count(22000, 20000));
}
