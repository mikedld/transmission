// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "transmission.h"
#include "quark.h"

#include "gtest/gtest.h"

#include <cstring>
#include <string>
#include <string_view>

class QuarkTest : public ::testing::Test
{
protected:
    template<typename T>
    std::string quarkGetString(T i)
    {
        size_t len;
        char const* const str = tr_quark_get_string(tr_quark(i), &len);
        EXPECT_EQ(strlen(str), len);
        return std::string(str, len);
    }
};

TEST_F(QuarkTest, allPredefinedKeysCanBeLookedUp)
{
    for (size_t i = 0; i < TR_N_KEYS; ++i)
    {
        auto const str = quarkGetString(i);
        auto const q = tr_quark_lookup(str);
        EXPECT_TRUE(q);
        EXPECT_EQ(i, *q);
    }
}

TEST_F(QuarkTest, newQuarkByStringView)
{
    auto constexpr UniqueString = std::string_view{ "this string is not a predefined quark" };
    auto const q = tr_quark_new(UniqueString);
    auto len = size_t{};
    EXPECT_EQ(UniqueString, tr_quark_get_string(q, &len));
    EXPECT_EQ(std::size(UniqueString), len);
}
