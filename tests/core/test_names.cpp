// Copyright 2026 Marc Donovici
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>

#include "names.hpp"

using rmw_qnx::core::decode_name;
using rmw_qnx::core::encode_name;

TEST(Names, a_fully_qualified_topic_becomes_one_directory_entry) {
  EXPECT_EQ(encode_name("/robot/joint_states"), "%2Frobot%2Fjoint_states");
}

TEST(Names, ros_name_characters_read_unchanged_in_ls) {
  EXPECT_EQ(encode_name("chatter_2~{x}"), "chatter_2~{x}");
}

TEST(Names, a_percent_sign_is_escaped_so_decoding_is_unambiguous) {
  EXPECT_EQ(encode_name("a%2Fb"), "a%252Fb");
  EXPECT_EQ(decode_name("a%252Fb"), "a%2Fb");
}

TEST(Names, a_file_name_holding_a_slash_is_refused) { EXPECT_FALSE(decode_name("a/b").has_value()); }

TEST(Names, an_unknown_or_cut_escape_is_refused) {
  EXPECT_FALSE(decode_name("a%41").has_value());
  EXPECT_FALSE(decode_name("a%2").has_value());
  EXPECT_FALSE(decode_name("a%").has_value());
}

TEST(Names, the_empty_name_round_trips) { EXPECT_EQ(decode_name(encode_name("")), ""); }

// P7: encoding is injective, never yields '/', and decodes back, for random byte strings.
TEST(NamesProperty, encoding_round_trips_and_is_injective_for_random_names) {
  std::mt19937 rng(20261107);
  const std::string alphabet = "/%_~{}aZ09 .2F5";
  std::set<std::string> inputs, outputs;
  for (int n = 0; n < 5000; n++) {
    std::string s(rng() % 12, ' ');
    for (char &c : s) c = alphabet[rng() % alphabet.size()];
    const std::string e = encode_name(s);
    ASSERT_EQ(e.find('/'), std::string::npos) << s;
    ASSERT_EQ(decode_name(e), s) << s;
    if (inputs.insert(s).second) {
      ASSERT_TRUE(outputs.insert(e).second) << "two names share " << e;
    }
  }
}
