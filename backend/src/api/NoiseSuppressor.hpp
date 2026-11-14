#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace CAM::API {

    class NoiseSuppressor {
    public:
        static constexpr size_t kFftSize = 512;
        static constexpr size_t kHopSize = 256;
        static constexpr size_t kNumBins = (kFftSize / 2) + 1;
        static constexpr size_t kNoiseEstimationFrames = 5;

        static constexpr float kMinGain = 0.10F;
        static constexpr float kEpsilon = 1e-9F;

        static std::vector<float> process(std::span<const float> pcm_samples);

    private:
        static std::vector<float> build_hann_window();
    };

}