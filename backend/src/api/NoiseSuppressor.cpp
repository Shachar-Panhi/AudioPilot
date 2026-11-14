#include "NoiseSuppressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <kiss_fftr.h>
#include <memory>
#include <numbers>

namespace CAM::API {

    std::vector<float> NoiseSuppressor::build_hann_window() {
        std::vector<float> window(kFftSize);
        constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
        for (size_t i = 0; i < kFftSize; ++i) {
            window[i] = 0.5F * (1.0F - std::cos((kTwoPi * static_cast<float>(i)) / static_cast<float>(kFftSize)));
        }
        return window;
    }

    std::vector<float> NoiseSuppressor::process(std::span<const float> pcm_samples) {
        if (pcm_samples.size() < kFftSize) {
            return std::vector<float>(pcm_samples.begin(), pcm_samples.end());
        }

        using FftConfigHandle = std::unique_ptr<kiss_fftr_state, decltype(&std::free)>;

        FftConfigHandle fwd_cfg(kiss_fftr_alloc(static_cast<int>(kFftSize), 0, nullptr, nullptr), &std::free);
        FftConfigHandle inv_cfg(kiss_fftr_alloc(static_cast<int>(kFftSize), 1, nullptr, nullptr), &std::free);

        if (!fwd_cfg || !inv_cfg) {
            return std::vector<float>(pcm_samples.begin(), pcm_samples.end());
        }

        static const std::vector<float> window = build_hann_window();

        const size_t num_frames = ((pcm_samples.size() - kFftSize) / kHopSize) + 1;
        const size_t est_frames = std::min(num_frames, kNoiseEstimationFrames);

        std::vector<float> time_frame(kFftSize);
        std::vector<kiss_fft_cpx> spectrum(kNumBins);
        std::vector<float> noise_psd(kNumBins, 0.0F);

        for (size_t m = 0; m < est_frames; ++m) {
            const size_t start = m * kHopSize;
            for (size_t n = 0; n < kFftSize; ++n) {
                time_frame[n] = pcm_samples[start + n] * window[n];
            }

            kiss_fftr(fwd_cfg.get(), time_frame.data(), spectrum.data());

            for (size_t k = 0; k < kNumBins; ++k) {
                const float power = (spectrum[k].r * spectrum[k].r) + (spectrum[k].i * spectrum[k].i);
                noise_psd[k] += power / static_cast<float>(est_frames);
            }
        }

        std::vector<float> clean_pcm(pcm_samples.size(), 0.0F);
        constexpr float kInvFftSize = 1.0F / static_cast<float>(kFftSize);

        for (size_t m = 0; m < num_frames; ++m) {
            const size_t start = m * kHopSize;
            for (size_t n = 0; n < kFftSize; ++n) {
                time_frame[n] = pcm_samples[start + n] * window[n];
            }

            kiss_fftr(fwd_cfg.get(), time_frame.data(), spectrum.data());

            for (size_t k = 0; k < kNumBins; ++k) {
                const float power = (spectrum[k].r * spectrum[k].r) + (spectrum[k].i * spectrum[k].i);
                const float clean_power = std::max(power - noise_psd[k], 0.0F);
                const float gain = std::clamp(clean_power / (power + kEpsilon), kMinGain, 1.0F);

                spectrum[k].r *= gain;
                spectrum[k].i *= gain;
            }

            kiss_fftri(inv_cfg.get(), spectrum.data(), time_frame.data());

            for (size_t n = 0; n < kFftSize; ++n) {
                clean_pcm[start + n] += time_frame[n] * kInvFftSize;
            }
        }

        for (float& sample : clean_pcm) {
            sample = std::clamp(sample, -1.0F, 1.0F);
        }

        return clean_pcm;
    }

}