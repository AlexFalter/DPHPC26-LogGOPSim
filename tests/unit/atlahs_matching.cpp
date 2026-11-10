#include "lgs/MessageMatching.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

// A message to rank 3 with a complete ATLAHS identity, and the receive that matches it.
class AtlahsMatching : public testing::Test {
protected:
    AtlahsMatching() {
        message.target = 3;
        message.tag = 0;
        message.message_id = {true, UINT64_MAX, 123, std::uint64_t{1} << 40, 5, UINT64_MAX};
        receive.src = 3;
        receive.message_id = message.message_id;
        receive.offset = 7;
    }

    graph_node_properties message{};
    ruqelem_t receive{};
};

} // namespace

// Each identity field alone distinguishes messages, also with values wider than upstream's
// decimal tag encoding allows.
TEST_F(AtlahsMatching, EveryIdentityFieldDistinguishesMessages) {
    for (int component = 0; component < 5; ++component) {
        SCOPED_TRACE("identity field " + std::to_string(component));
        auto wrong = receive;
        switch (component) {
        case 0: --wrong.message_id.communicator; break;
        case 1: --wrong.message_id.channel; break;
        case 2: --wrong.message_id.sequence; break;
        case 3: --wrong.message_id.kind; break;
        case 4: --wrong.message_id.chunk; break;
        }
        wrong.offset = 0; // A tempting earlier receive must remain unmatched.
        ruq_t queue{wrong, receive};
        ruqelem_t result;
        EXPECT_EQ(match(message, &queue, &result), 2);
        EXPECT_EQ(result.offset, 7);
        ASSERT_EQ(queue.size(), 1u);
        EXPECT_EQ(queue.front().offset, 0);
    }
}

TEST_F(AtlahsMatching, ChecksTheSourceUnlessItIsAny) {
    auto wrong_source = receive;
    wrong_source.src = 4;
    ruq_t sources{wrong_source};
    EXPECT_EQ(match(message, &sources), -1);
    EXPECT_EQ(sources.size(), 1u);
    message.target = ANY_SOURCE;
    EXPECT_GT(match(message, &sources), 0);
    EXPECT_TRUE(sources.empty());
}

// Explicit endpoint pairs disambiguate otherwise identical message identities.
TEST_F(AtlahsMatching, ChecksBothGpus) {
    message.gpu = receive.gpu = 2;
    message.peer_gpu = receive.peer_gpu = 0;
    for (bool wrong_local : {false, true}) {
        SCOPED_TRACE(wrong_local ? "wrong local GPU" : "wrong peer GPU");
        auto wrong = receive;
        if (wrong_local) wrong.gpu = 1;
        else wrong.peer_gpu = 1;
        ruq_t endpoints{wrong, receive};
        ruqelem_t chosen;
        EXPECT_GT(match(message, &endpoints, &chosen), 0);
        EXPECT_EQ(endpoints.size(), 1u);
        EXPECT_EQ(chosen.gpu, 2);
        EXPECT_EQ(chosen.peer_gpu, 0);
    }
}

// The lowest operation index wins, whatever the insertion order.
TEST_F(AtlahsMatching, PicksTheEarliestEligibleReceive) {
    auto first = receive, second = receive, third = receive;
    first.offset = 9;
    second.offset = 2;
    third.offset = 5;
    ruq_t ordered{first, second, third};
    ruqelem_t result;
    EXPECT_GT(match(message, &ordered, &result), 0);
    EXPECT_EQ(result.offset, 2);
}

TEST_F(AtlahsMatching, KeepsLegacyAndExplicitMatchingApart) {
    graph_node_properties legacy{};
    legacy.target = 3;
    legacy.tag = 42;
    auto legacy_receive = receive;
    legacy_receive.message_id = {};
    legacy_receive.tag = ANY_TAG;
    ruq_t mixed{receive, legacy_receive};
    ruqelem_t result;
    EXPECT_GT(match(legacy, &mixed, &result), 0);
    EXPECT_FALSE(result.message_id.explicit_fields);
    EXPECT_EQ(mixed.size(), 1u);
    EXPECT_EQ(match(legacy, &mixed), -1) << "a legacy message matched an explicit receive";
}
