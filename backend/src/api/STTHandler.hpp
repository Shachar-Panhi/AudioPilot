#pragma once

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <whisper.h>
#include "NoiseSuppressor.hpp"

namespace CAM::API {
    class STTHandler : public std::enable_shared_from_this<STTHandler> {
    public:
        using ContextHandle = std::unique_ptr<whisper_context, decltype(&whisper_free)>;
        using TranscribeCallback = std::function<void(std::expected<std::string, std::string>)>;

        static constexpr const char* kInitialPrompt =
            "Commands: reset, Reset. Mixing 15, mixing 80. AGC 0, AGC 10. Pitch-shift 0, Pitch-Shift -6, Pitch shift +3";

        // voice commands are a handful of words. capping the output bounds latency if the model starts looping
        static constexpr int kMaxTokens = 32;

        // whisper always encodes a 30s window (1500 encoder frames, 20ms each) even for a 2s command.
        // shrinking the encoder context to the clip length (plus headroom, which the model needs to avoid
        // repeating itself) makes encoding several times faster for short PTT clips.
        static constexpr int kSamplesPerEncoderFrame = 320;
        static constexpr int kAudioCtxMargin = 256;
        static constexpr int kMinAudioCtx = 384;

        static std::expected<std::shared_ptr<STTHandler>, std::string>
        create(const std::filesystem::path& model_path,
               const std::filesystem::path& vad_model_path = {},
               bool use_gpu = true,
               bool enable_noise_suppression = false);

        ~STTHandler();

        STTHandler(const STTHandler&) = delete;
        STTHandler& operator=(const STTHandler&) = delete;

        STTHandler(STTHandler&&) = delete;
        STTHandler& operator=(STTHandler&&) = delete;

        // takes ownership of the samples so noise suppression can run in place without copying the clip
        std::expected<std::string, std::string>
        transcribe(std::vector<float> pcm_samples);

        void transcribe_async(std::vector<float> pcm_samples, TranscribeCallback callback);

    private:
        STTHandler(ContextHandle context, std::string vad_model_path, std::optional<NoiseSuppressor> noise_suppressor) noexcept;

        whisper_full_params make_params(size_t num_samples) const;

        ContextHandle context_;
        std::string vad_model_path_;
        std::optional<NoiseSuppressor> noise_suppressor_;
        int n_threads_;
        std::mutex mutex_;
        boost::asio::thread_pool worker_pool_{1};
    };
}