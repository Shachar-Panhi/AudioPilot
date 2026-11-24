#include "api/OpusTranscoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

using CAM::API::OpusTranscoder;

namespace {
    using Frame = std::array<float, OpusTranscoder::kDecodedPacketSize>;

    constexpr float kToneFrequency = 440.0F;
    constexpr float kToneAmplitude = 0.5F;

    // one frame of a continuous sine wave, starting at sample `offset`
    Frame tone_frame(size_t offset) {
        Frame frame{};
        for (auto [index, sample] : std::views::enumerate(frame)) {
            const auto time = static_cast<float>(offset + static_cast<size_t>(index)) /
                              static_cast<float>(OpusTranscoder::kSampleRate);
            sample = kToneAmplitude * std::sin(2.0F * std::numbers::pi_v<float> * kToneFrequency * time);
        }
        return frame;
    }

    float rms(std::span<const float> samples) {
        double sum = 0.0;
        for (const float sample : samples) {
            sum += static_cast<double>(sample) * sample;
        }
        return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
    }

    OpusTranscoder make_transcoder() {
        auto transcoder = OpusTranscoder::create();
        REQUIRE(transcoder);
        return std::move(*transcoder);
    }
}

TEST_CASE("The transcoder can be created", "[OpusTranscoder]") {
    CHECK(OpusTranscoder::create().has_value());
}

TEST_CASE("Encoding rejects frames of the wrong size", "[OpusTranscoder]") {
    auto transcoder = make_transcoder();

    SECTION("too short") {
        const std::vector<float> samples(OpusTranscoder::kDecodedPacketSize - 1);
        CHECK_FALSE(transcoder.encode(samples));
    }

    SECTION("too long") {
        const std::vector<float> samples(OpusTranscoder::kDecodedPacketSize + 1);
        CHECK_FALSE(transcoder.encode(samples));
    }

    SECTION("empty") {
        CHECK_FALSE(transcoder.encode({}));
    }
}

TEST_CASE("Audio survives an encode/decode round trip", "[OpusTranscoder]") {
    auto transcoder = make_transcoder();
    // enough frames for the codec's start-up delay to pass
    constexpr size_t kFrames = 25;

    float input_rms = 0.0F;
    float output_rms = 0.0F;
    for (size_t index = 0; index < kFrames; ++index) {
        const Frame input = tone_frame(index * OpusTranscoder::kDecodedPacketSize);
        const auto encoded = transcoder.encode(input);
        REQUIRE(encoded);
        const auto decoded = transcoder.decode(*encoded);
        REQUIRE(decoded);
        input_rms = rms(input);
        output_rms = rms(*decoded);
    }

    // lossy, so compare loudness rather than samples
    CHECK(output_rms > input_rms * 0.5F);
    CHECK(output_rms < input_rms * 1.5F);
}

TEST_CASE("Silence decodes to near-silence", "[OpusTranscoder]") {
    auto transcoder = make_transcoder();
    const Frame silence{};

    for (int index = 0; index < 5; ++index) {
        const auto encoded = transcoder.encode(silence);
        REQUIRE(encoded);
        const auto decoded = transcoder.decode(*encoded);
        REQUIRE(decoded);
        CHECK(rms(*decoded) < 1e-3F);
    }
}

TEST_CASE("An empty packet runs loss concealment and returns a full frame", "[OpusTranscoder]") {
    auto transcoder = make_transcoder();

    const auto encoded = transcoder.encode(tone_frame(0));
    REQUIRE(encoded);
    REQUIRE(transcoder.decode(*encoded));

    const auto concealed = transcoder.decode({});
    REQUIRE(concealed);
    CHECK(concealed->size() == OpusTranscoder::kDecodedPacketSize);
}

TEST_CASE("A malformed packet is an error", "[OpusTranscoder]") {
    auto transcoder = make_transcoder();
    // code 3 packet (multiple frames) that declares zero frames, which the Opus spec forbids
    const std::array<uint8_t, 2> malformed = {0x03, 0x00};
    CHECK_FALSE(transcoder.decode(malformed));
}

TEST_CASE("The decoder keeps working after a reset", "[OpusTranscoder]") {
    auto transcoder = make_transcoder();
    const auto encoded = transcoder.encode(tone_frame(0));
    REQUIRE(encoded);
    REQUIRE(transcoder.decode(*encoded));

    transcoder.reset_decoder();

    const auto decoded = transcoder.decode(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->size() == OpusTranscoder::kDecodedPacketSize);
}
