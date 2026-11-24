#include "api/NoiseSuppressor.hpp"
#include "api/OpusTranscoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <utility>
#include <vector>

using CAM::API::NoiseSuppressor;

namespace {
    constexpr size_t kSampleRate = CAM::API::OpusTranscoder::kSampleRate;
    constexpr size_t kClipSamples = kSampleRate;          // 1s
    constexpr size_t kReferenceSamples = kSampleRate / 2; // 0.5s
    constexpr float kNoiseAmplitude = 0.05F;

    // fixed seed so every run sees the same noise
    std::vector<float> white_noise(size_t count, unsigned seed, float amplitude = kNoiseAmplitude) {
        std::mt19937 generator(seed);
        std::uniform_real_distribution<float> distribution(-amplitude, amplitude);
        std::vector<float> samples(count);
        std::ranges::generate(samples, [&] { return distribution(generator); });
        return samples;
    }

    std::vector<float> sine(size_t count, float frequency, float amplitude) {
        std::vector<float> samples(count);
        for (size_t i = 0; i < count; ++i) {
            const auto time = static_cast<float>(i) / static_cast<float>(kSampleRate);
            samples[i] = amplitude * std::sin(2.0F * std::numbers::pi_v<float> * frequency * time);
        }
        return samples;
    }

    float rms(std::span<const float> samples) {
        double sum = 0.0;
        for (const float sample : samples) {
            sum += static_cast<double>(sample) * sample;
        }
        return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
    }

    NoiseSuppressor make_suppressor() {
        auto suppressor = NoiseSuppressor::create();
        REQUIRE(suppressor);
        return std::move(*suppressor);
    }
}

TEST_CASE("The noise suppressor can be created", "[NoiseSuppressor]") {
    CHECK(NoiseSuppressor::create().has_value());
}

TEST_CASE("Clips too short to measure are left untouched", "[NoiseSuppressor]") {
    auto suppressor = make_suppressor();

    SECTION("empty clip") {
        std::vector<float> samples;
        suppressor.process(samples);
        CHECK(samples.empty());
    }

    SECTION("a handful of samples") {
        auto samples = white_noise(16, 1);
        const auto original = samples;
        suppressor.process(samples);
        CHECK(samples == original);
    }
}

TEST_CASE("Silence stays silent", "[NoiseSuppressor]") {
    auto suppressor = make_suppressor();
    std::vector<float> samples(kClipSamples, 0.0F);
    suppressor.process(samples);
    CHECK(std::ranges::all_of(samples, [](float sample) { return sample == 0.0F; }));
}

TEST_CASE("Steady noise is reduced", "[NoiseSuppressor]") {
    auto suppressor = make_suppressor();
    auto samples = white_noise(kClipSamples, 2);
    const float input_rms = rms(samples);

    SECTION("with a noise reference") {
        const auto reference = white_noise(kReferenceSamples, 3);
        suppressor.process(samples, reference);
        CHECK(rms(samples) < input_rms * 0.8F);
    }

    SECTION("without a noise reference") {
        suppressor.process(samples);
        CHECK(rms(samples) < input_rms * 0.8F);
    }
}

TEST_CASE("Speech over noise is kept while the noise around it is reduced", "[NoiseSuppressor]") {
    auto suppressor = make_suppressor();

    // a tone in the middle half of the clip stands in for speech. a tone that never stops would look like hum
    const size_t speech_start = kClipSamples / 4;
    const size_t speech_end = 3 * kClipSamples / 4;
    const auto tone = sine(kClipSamples, 1000.0F, 0.3F);
    auto samples = white_noise(kClipSamples, 4);
    for (size_t i = speech_start; i < speech_end; ++i) {
        samples[i] += tone[i];
    }
    const auto noisy = samples;
    const auto reference = white_noise(kReferenceSamples, 5);

    suppressor.process(samples, reference);

    const auto speech = std::span(samples).subspan(speech_start, speech_end - speech_start);
    const auto clean_speech = std::span(tone).subspan(speech_start, speech_end - speech_start);
    const auto quiet_part = std::span(samples).first(speech_start);
    const auto noisy_quiet_part = std::span(noisy).first(speech_start);

    // the tone's loudness is mostly kept
    CHECK(rms(speech) > rms(clean_speech) * 0.8F);
    // and the noise-only part gets quieter
    CHECK(rms(quiet_part) < rms(noisy_quiet_part) * 0.8F);
}

TEST_CASE("Output stays within [-1, 1]", "[NoiseSuppressor]") {
    auto suppressor = make_suppressor();
    // loud noise with a full-scale tone on top, so the rebuilt audio overshoots before clamping
    auto samples = white_noise(kClipSamples, 6, 0.5F);
    const auto tone = sine(kClipSamples, 200.0F, 1.0F);
    for (size_t i = kClipSamples / 2; i < kClipSamples; ++i) {
        samples[i] = std::clamp(samples[i] + tone[i], -1.0F, 1.0F);
    }

    suppressor.process(samples);

    CHECK(std::ranges::all_of(samples, [](float sample) { return sample >= -1.0F && sample <= 1.0F; }));
}

TEST_CASE("The suppressor can be reused across clips of different lengths", "[NoiseSuppressor]") {
    auto suppressor = make_suppressor();

    auto long_clip = white_noise(kClipSamples, 7);
    suppressor.process(long_clip, white_noise(kReferenceSamples, 8));

    // a shorter clip after a longer one: buffers from the first call must not leak into the second
    auto short_clip = white_noise(kClipSamples / 4, 9);
    auto fresh_clip = short_clip;
    suppressor.process(short_clip);
    make_suppressor().process(fresh_clip);

    CHECK(short_clip == fresh_clip);
}
