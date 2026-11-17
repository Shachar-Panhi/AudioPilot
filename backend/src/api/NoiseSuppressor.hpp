#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <kiss_fftr.h>

namespace CAM::API {

    // Removes steady background noise (hum, hiss, fans) from a complete PTT clip (16kHz mono).
    //
    // How it works:
    //   1. Split the clip into short overlapping frames and convert each one to frequencies (FFT).
    //   2. For each frequency, treat its quietest moments in the clip as the noise level.
    //   3. Turn down each frequency in each frame by how much of it is noise (never by more than -14dB).
    //   4. Convert the frames back to audio and stitch them together.
    //
    class NoiseSuppressor {
    public:
        static std::expected<NoiseSuppressor, std::string> create();

        ~NoiseSuppressor() = default;

        NoiseSuppressor(const NoiseSuppressor&) = delete;
        NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

        NoiseSuppressor(NoiseSuppressor&&) noexcept = default;
        NoiseSuppressor& operator=(NoiseSuppressor&&) noexcept = default;

        // denoises the samples in place
        void process(std::span<float> pcm_samples);

    private:
        static constexpr size_t kFftSize = 512;                // 32ms frames
        static constexpr size_t kHopSize = kFftSize / 2;       // frames overlap by half
        static constexpr size_t kNumBins = (kFftSize / 2) + 1; // frequencies per frame

        // kiss_fftr_free is a macro, so it needs wrapping to be used as a deleter
        struct FftDeleter {
            void operator()(kiss_fftr_cfg cfg) const noexcept { kiss_fftr_free(cfg); } //NOLINT
        };
        using FftPlan = std::unique_ptr<kiss_fftr_state, FftDeleter>;

        NoiseSuppressor(FftPlan forward, FftPlan inverse) noexcept;

        void compute_spectra(std::span<const float> pcm_samples, size_t num_frames);
        void estimate_noise(size_t num_frames);
        void apply_gains(size_t num_frames);
        void rebuild_audio(std::span<float> pcm_samples, size_t num_frames);

        std::span<kiss_fft_cpx, kNumBins> frame_spectrum(size_t frame);

        FftPlan forward_;
        FftPlan inverse_;
        std::array<float, kFftSize> window_{};

        // reused between calls so processing doesn't allocate every time
        std::vector<float> input_;           // the clip with silence added at both ends
        std::vector<float> output_;          // rebuilt audio, same layout as input_
        std::vector<kiss_fft_cpx> spectra_;  // every frame's spectrum, one after another
        std::vector<float> bin_history_;     // one frequency's power across all frames
        std::array<float, kNumBins> noise_{};  // noise power per frequency
        std::array<float, kFftSize> frame_{};  // one windowed frame
    };

}
