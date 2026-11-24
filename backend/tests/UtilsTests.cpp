#include "utils/Utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

using CAM::Utils::generate_session_id;

TEST_CASE("Session IDs are UUID strings", "[Utils]") {
    const std::string session_id = generate_session_id();

    // 8-4-4-4-12 hex digits
    REQUIRE(session_id.size() == 36);
    for (const size_t dash : {8, 13, 18, 23}) {
        CHECK(session_id[dash] == '-');
    }
    std::string digits = session_id;
    std::erase(digits, '-');
    CHECK(std::ranges::all_of(digits, [](unsigned char digit) { return std::isxdigit(digit) != 0; }));
}

TEST_CASE("Session IDs are unique", "[Utils]") {
    std::set<std::string> ids;
    for (int index = 0; index < 1000; ++index) {
        ids.insert(generate_session_id());
    }
    CHECK(ids.size() == 1000);
}
