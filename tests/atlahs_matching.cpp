#include "lgs/MessageMatching.hpp"

#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    graph_node_properties message{};
    message.target = 3;
    message.tag = 0;
    message.message_id = {true, UINT64_MAX, 123, std::uint64_t{1} << 40, 5, UINT64_MAX};
    ruqelem_t receive{};
    receive.src = 3;
    receive.message_id = message.message_id;
    receive.offset = 7;
    // Each field alone distinguishes messages, also with values wider than
    // upstream's decimal tag encoding allows.
    for (int component = 0; component < 5; ++component) {
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
        check(match(message, &queue, &result) == 2 && result.offset == 7 && queue.size() == 1 &&
              queue.front().offset == 0, "A distinct message identity matched the wrong receive");
    }
    auto wrong_source = receive;
    wrong_source.src = 4;
    ruq_t sources{wrong_source};
    check(match(message, &sources) == -1 && sources.size() == 1, "Source was ignored");
    message.target = ANY_SOURCE;
    check(match(message, &sources) > 0 && sources.empty(), "Source wildcard was lost");

    // Explicit endpoint pairs disambiguate otherwise identical message identities.
    message.target = 3;
    message.gpu = receive.gpu = 2;
    message.peer_gpu = receive.peer_gpu = 0;
    for (bool wrong_local : {false, true}) {
        auto wrong = receive;
        if (wrong_local) wrong.gpu = 1; else wrong.peer_gpu = 1;
        ruq_t endpoints{wrong, receive};
        ruqelem_t chosen;
        check(match(message, &endpoints, &chosen) > 0 && endpoints.size() == 1 &&
              chosen.gpu == 2 && chosen.peer_gpu == 0,
              "Explicit matching ignored the source or destination GPU");
    }
    message.gpu = message.peer_gpu = receive.gpu = receive.peer_gpu = 0;

    // Lowest operation index wins even when insertion order differs.
    message.target = 3;
    auto first = receive, second = receive, third = receive;
    first.offset = 9; second.offset = 2; third.offset = 5;
    ruq_t ordered{first, second, third};
    ruqelem_t result;
    check(match(message, &ordered, &result) > 0 && result.offset == 2,
          "Matching did not select the earliest eligible operation");

    graph_node_properties legacy{};
    legacy.target = 3; legacy.tag = 42;
    auto legacy_receive = receive;
    legacy_receive.message_id = {};
    legacy_receive.tag = ANY_TAG;
    ruq_t mixed{receive, legacy_receive};
    check(match(legacy, &mixed, &result) > 0 && !result.message_id.explicit_fields && mixed.size() == 1,
          "Legacy wildcard or separation between matching modes failed");
    check(match(legacy, &mixed) == -1, "Legacy and explicit identities were mixed");
    std::cout << "PASS: complete ATLAHS identities, wide values, wildcards and receive ordering\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
