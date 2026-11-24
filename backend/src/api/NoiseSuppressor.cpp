#include "NoiseSuppressor.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <ranges>
#include <utility>

namespace CAM::API {

    namespace {
        // clips shorter than this many frames (~130ms) are too short to measure noise in, so they're left alone
        constexpr size_t kMinFrames = 8;

        // without a noise reference, a frequency's noise level is taken from its quietest 10% of frames.
        // it's also a safety cap when there is a reference, in case the user was talking just before PTT
        constexpr float kNoiseQuantile = 0.1F;

        // how much the speech estimate carries over from the previous frame. smoothing avoids the "musical noise"
        // (random chirps) of per-frame noise removal, but too much of it lags behind and clips the start of words
        constexpr float kSmoothing = 0.9F;

        // a frequency is never turned down below 25% (-12dB). tuned on voice commands in white/fan/background-talker
        // noise: -6dB and -9dB left more noise for Whisper to misread, and going beyond -12dB gained nothing more
        constexpr float kMinGain = 0.25F;
        constexpr float kMinSpeechToNoise = 0.0032F; // -25dB
        constexpr float kEpsilon = 1e-12F;            // avoids dividing by zero in silent clips

        float power(const kiss_fft_cpx& bin) {
            return (bin.r * bin.r) + (bin.i * bin.i);
        }
    }

    NoiseSuppressor::NoiseSuppressor(FftPlan forward, FftPlan inverse) noexcept
        : forward_(std::move(forward)),
          inverse_(std::move(inverse)) {
        // square root of a Hann window. it's applied when splitting and again when stitching, and with
        // half-overlapping frames the two together add back up to exactly 1, so untouched audio comes out unchanged
        for (auto [index, value] : std::views::enumerate(window_)) {
            value = std::sin((std::numbers::pi_v<float> * static_cast<float>(index)) / static_cast<float>(kFftSize));
        }
    }

    std::expected<NoiseSuppressor, std::string> NoiseSuppressor::create() {
        FftPlan forward(kiss_fftr_alloc(static_cast<int>(kFftSize), 0, nullptr, nullptr));
        FftPlan inverse(kiss_fftr_alloc(static_cast<int>(kFftSize), 1, nullptr, nullptr));

        if (!forward || !inverse) {
            return std::unexpected("NoiseSuppressor: failed to allocate kissfft plans");
        }

        return NoiseSuppressor(std::move(forward), std::move(inverse));
    }

    void NoiseSuppressor::process(std::span<float> pcm_samples, std::span<const float> noise_reference) {
        // the reference is processed together with the clip. besides measuring the noise, this means the
        // speech estimate has already settled by the time the clip starts, so its first word isn't clipped.
        // one extra frame so every sample is covered by two frames, including the first and last ones
        reference_size_ = noise_reference.size();
        num_frames_ = ((reference_size_ + pcm_samples.size() + kHopSize - 1) / kHopSize) + 1;
        if (num_frames_ < kMinFrames) {
            return;
        }

        compute_spectra(noise_reference, pcm_samples);
        estimate_noise();
        apply_gains();
        rebuild_audio(pcm_samples);
    }

    std::span<kiss_fft_cpx, NoiseSuppressor::kNumBins> NoiseSuppressor::frame_spectrum(size_t frame) {
        return std::span(spectra_).subspan(frame * kNumBins).first<kNumBins>();
    }

    void NoiseSuppressor::compute_spectra(std::span<const float> noise_reference, std::span<const float> pcm_samples) {
        // half a frame of silence at the start (and at least that much at the end) so the edges aren't faded out
        input_.assign(((num_frames_ - 1) * kHopSize) + kFftSize, 0.0F);
        const auto reference_end = std::ranges::copy(noise_reference, input_.begin() + kHopSize).out;
        std::ranges::copy(pcm_samples, reference_end);
        spectra_.resize(num_frames_ * kNumBins);

        for (size_t frame = 0; frame < num_frames_; ++frame) {
            const auto samples = std::span(input_).subspan(frame * kHopSize, kFftSize);
            std::ranges::transform(samples, window_, frame_.begin(), std::multiplies{});
            kiss_fftr(forward_.get(), frame_.data(), frame_spectrum(frame).data());
        }
    }

    void NoiseSuppressor::estimate_noise() {
        // skip the first and last frames: they're half silence and would make the noise look quieter than it is
        const size_t count = num_frames_ - 2;
        const auto rank = static_cast<std::ptrdiff_t>(kNoiseQuantile * static_cast<float>(count - 1));

        // the 10% mark of pure noise sits below its average. this factor scales it back up to the average
        const float to_average = -1.0F / std::log1p(-kNoiseQuantile);

        bin_history_.resize(count);
        for (auto [bin, noise] : std::views::enumerate(noise_)) {
            // frame + 1 because the first frame is skipped
            for (auto [frame, history] : std::views::enumerate(bin_history_)) {
                history = power(spectra_[((static_cast<size_t>(frame) + 1) * kNumBins) + static_cast<size_t>(bin)]);
            }

            std::ranges::nth_element(bin_history_, bin_history_.begin() + rank);
            noise = (bin_history_[static_cast<size_t>(rank)] * to_average) + kEpsilon;
        }

        if (reference_size_ < kFftSize) {
            return;
        }

        // the reference holds only noise, so its average is the noise level. frames 1..reference_frames lie
        // entirely inside it (frame 0 is half silence). keeping the lower of the two estimates protects the
        // speech if the "reference" actually contained the user talking
        const size_t reference_frames = ((reference_size_ - kFftSize) / kHopSize) + 1;
        for (auto [bin, noise] : std::views::enumerate(noise_)) {
            double reference_noise = 0.0;
            for (size_t frame = 1; frame <= reference_frames; ++frame) {
                reference_noise += power(frame_spectrum(frame)[static_cast<size_t>(bin)]);
            }
            reference_noise /= static_cast<double>(reference_frames);
            noise = std::min(noise, static_cast<float>(reference_noise) + kEpsilon);
        }
    }

    void NoiseSuppressor::apply_gains() {
        // estimated speech-to-noise ratio of each frequency in the previous frame
        std::array<float, kNumBins> prev_speech_to_noise{};

        for (size_t frame = 0; frame < num_frames_; ++frame) {
            // nothing to carry over into the first frame
            const float smoothing = frame == 0 ? 0.0F : kSmoothing;

            for (auto&& [bin, noise, prev] : std::views::zip(frame_spectrum(frame), noise_, prev_speech_to_noise)) {
                const float signal_to_noise = power(bin) / noise;

                // blend last frame's speech estimate with this frame's (whatever is above the noise level)
                const float speech_to_noise = std::max(
                    (smoothing * prev) + ((1.0F - smoothing) * std::max(signal_to_noise - 1.0F, 0.0F)),
                    kMinSpeechToNoise);

                // Wiener gain: the fraction of this frequency that is speech
                const float gain = speech_to_noise / (1.0F + speech_to_noise);
                prev = gain * gain * signal_to_noise;

                const float applied_gain = std::max(gain, kMinGain);
                bin.r *= applied_gain;
                bin.i *= applied_gain;
            }
        }
    }

    void NoiseSuppressor::rebuild_audio(std::span<float> pcm_samples) {
        constexpr float kInvFftSize = 1.0F / static_cast<float>(kFftSize); // kissfft's inverse doesn't rescale

        output_.assign(input_.size(), 0.0F);

        for (size_t frame = 0; frame < num_frames_; ++frame) {
            kiss_fftri(inverse_.get(), frame_spectrum(frame).data(), frame_.data());

            const auto samples = std::span(output_).subspan(frame * kHopSize, kFftSize);
            for (auto&& [out, sample, win] : std::views::zip(samples, frame_, window_)) {
                out += sample * win * kInvFftSize;
            }
        }

        // drop the added silence and the reference, and keep samples in the valid range
        const auto denoised = std::span(output_).subspan(kHopSize + reference_size_, pcm_samples.size());
        std::ranges::transform(denoised, pcm_samples.begin(),
                               [](float sample) { return std::clamp(sample, -1.0F, 1.0F); });
    }

}
