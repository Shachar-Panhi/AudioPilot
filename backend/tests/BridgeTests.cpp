#include "api/Bridge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using CAM::API::RtpRewriteState;

namespace {
    using Packet = std::vector<std::byte>;

    constexpr uint32_t kSamplesPerPacket = 960; // 20ms at the 48kHz opus RTP clock

    // a 12 byte RTP header (version 2) followed by a 4 byte payload
    Packet make_packet(uint16_t seq, uint32_t timestamp) {
        Packet packet(16, std::byte{0xAB});
        packet[0] = std::byte{0x80};
        packet[1] = std::byte{111};
        for (size_t i = 0; i < 2; ++i) {
            packet[2 + i] = static_cast<std::byte>(seq >> (8U * (1 - i)));
        }
        for (size_t i = 0; i < 4; ++i) {
            packet[4 + i] = static_cast<std::byte>(timestamp >> (8U * (3 - i)));
        }
        return packet;
    }

    uint32_t read_field(const Packet& packet, size_t offset, size_t size) {
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i) {
            value = (value << 8U) | std::to_integer<uint32_t>(packet[offset + i]);
        }
        return value;
    }

    // rewrites a packet and returns the {sequence number, timestamp} the partner would get
    std::pair<uint32_t, uint32_t> forward(RtpRewriteState& state, uint16_t seq, uint32_t timestamp) {
        auto packet = make_packet(seq, timestamp);
        REQUIRE(state.rewrite(packet));
        return {read_field(packet, 2, 2), read_field(packet, 4, 4)};
    }

    // holds back `count` consecutive 20ms packets starting at (seq, timestamp)
    void suppress(RtpRewriteState& state, uint16_t seq, uint32_t timestamp, int count) {
        for (int i = 0; i < count; ++i) {
            auto packet = make_packet(static_cast<uint16_t>(seq + i), timestamp + (static_cast<uint32_t>(i) * kSamplesPerPacket));
            REQUIRE(state.suppress(packet));
        }
    }
}

TEST_CASE("RtpRewriteState leaves an uninterrupted stream unchanged", "[RtpRewriteState]") {
    RtpRewriteState state;
    forward(state, 100, 5000);
    auto packet = make_packet(101, 5000 + kSamplesPerPacket);
    const Packet original = packet;
    REQUIRE(state.rewrite(packet));
    CHECK(packet == original);
}

TEST_CASE("RtpRewriteState cuts a PTT gap out of the stream", "[RtpRewriteState]") {
    RtpRewriteState state;
    forward(state, 100, 0);
    suppress(state, 101, kSamplesPerPacket, 50);
    // continues straight on from the packet before PTT
    CHECK(forward(state, 151, 51 * kSamplesPerPacket) == std::pair<uint32_t, uint32_t>{101, kSamplesPerPacket});
}

TEST_CASE("RtpRewriteState adds up several PTT gaps", "[RtpRewriteState]") {
    RtpRewriteState state;
    forward(state, 0, 0);
    suppress(state, 1, kSamplesPerPacket, 10);
    CHECK(forward(state, 11, 11 * kSamplesPerPacket) == std::pair<uint32_t, uint32_t>{1, kSamplesPerPacket});
    suppress(state, 12, 12 * kSamplesPerPacket, 25);
    CHECK(forward(state, 37, 37 * kSamplesPerPacket) == std::pair<uint32_t, uint32_t>{2, 2 * kSamplesPerPacket});
}

TEST_CASE("RtpRewriteState handles wraparound", "[RtpRewriteState]") {
    RtpRewriteState state;

    SECTION("sequence numbers") {
        forward(state, 65534, 0);
        suppress(state, 65535, kSamplesPerPacket, 5); // 65535, 0, 1, 2, 3
        CHECK(forward(state, 4, 6 * kSamplesPerPacket).first == 65535);
        CHECK(forward(state, 5, 7 * kSamplesPerPacket).first == 0);
    }

    SECTION("timestamps") {
        const uint32_t near_wrap = UINT32_MAX - kSamplesPerPacket + 1; // the next packet wraps to 0
        forward(state, 1, near_wrap);
        suppress(state, 2, 0, 5);
        CHECK(forward(state, 7, 5 * kSamplesPerPacket).second == 0);
    }
}

TEST_CASE("RtpRewriteState rejects malformed RTP", "[RtpRewriteState]") {
    RtpRewriteState state;
    forward(state, 0, 0);
    suppress(state, 1, kSamplesPerPacket, 2);
    const RtpRewriteState before = state;

    auto packet = make_packet(3, 3 * kSamplesPerPacket);
    SECTION("shorter than the 12 byte header") {
        packet.resize(11);
    }
    SECTION("wrong RTP version") {
        packet[0] = std::byte{0x40};
    }

    const Packet original = packet;
    CHECK_FALSE(state.rewrite(packet));
    CHECK_FALSE(state.suppress(packet));
    CHECK(packet == original);
    CHECK(state.sequence_offset_ == before.sequence_offset_);
    CHECK(state.timestamp_offset_ == before.timestamp_offset_);
}
