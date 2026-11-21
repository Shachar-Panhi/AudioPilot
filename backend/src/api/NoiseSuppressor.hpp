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
    //   2. Measure each frequency's noise level from the audio recorded just before the clip (the noise reference),
    //      or from the clip's quietest moments when there is no reference.
    //   3. Turn down each frequency in each frame by how much of it is noise (never by more than -12dB).
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

        // denoises the samples in place. noise_reference is the audio right before the clip (e.g. the moment
        // before PTT was pressed). it's only used to measure the noise and is never modified.
        void process(std::span<float> pcm_samples, std::span<const float> noise_reference = {});

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

        void compute_spectra(std::span<const float> noise_reference, std::span<const float> pcm_samples);
        void estimate_noise();
        void apply_gains();
        void rebuild_audio(std::span<float> pcm_samples);

        std::span<kiss_fft_cpx, kNumBins> frame_spectrum(size_t frame);

        FftPlan forward_;
        FftPlan inverse_;
        std::array<float, kFftSize> window_{};

        // set at the start of each process() call
        size_t reference_size_ = 0;
        size_t num_frames_ = 0;

        // reused between calls so processing doesn't allocate every time
        std::vector<float> input_;           // silence, noise reference, clip, silence
        std::vector<float> output_;          // rebuilt audio, same layout as input_
        std::vector<kiss_fft_cpx> spectra_;  // every frame's spectrum, one after another
        std::vector<float> bin_history_;     // one frequency's power across all frames
        std::array<float, kNumBins> noise_{};  // noise power per frequency
        std::array<float, kFftSize> frame_{};  // one windowed frame
    };

}
