#pragma once

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whisper.h>
#include "CommandParser.hpp"
#include "NoiseSuppressor.hpp"

namespace CAM::API {
    class STTHandler : public std::enable_shared_from_this<STTHandler> {
    public:
        using ContextHandle = std::unique_ptr<whisper_context, decltype(&whisper_free)>;
        struct Transcript {
            std::string text; //NOLINT
            // mean token probability of the first sentence (the command). when whisper doesn't really hear the
            // audio it tends to echo a line of the prompt instead, and those echoes score ~0.5, real commands ~0.95
            float confidence = 0.0F; //NOLINT
        };
        using TranscribeCallback = std::function<void(std::expected<Transcript, std::string>)>;

        // commands transcribed with lower confidence are rejected (the user is asked to repeat) rather than run.
        // in testing, 0.5 blocked about a third of the remaining wrong commands while asking the user to repeat
        // <1% of correct ones. higher values block more, but reject many short commands like "Reset"
        static constexpr float kMinCommandConfidence = 0.5F;

        // voice commands are a handful of words. capping the output bounds latency if the model starts looping
        static constexpr int kMaxTokens = 16;

        // speech kept around each region the VAD detects. the default (30ms) clipped the quiet start of short
        // commands ("A G C" became "GC", "Reset" became "We set"); 200ms fixed most of it in testing
        static constexpr int kVadSpeechPadMs = 200;

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

        // takes ownership of the samples so noise suppression can run in place without copying the clip.
        // noise_reference is the audio recorded just before the clip. the noise suppressor measures the noise from
        // it, but it is not transcribed
        std::expected<Transcript, std::string>
        transcribe(std::vector<float> pcm_samples, std::span<const float> noise_reference = {});

        void transcribe_async(std::vector<float> pcm_samples, std::vector<float> noise_reference,
                              TranscribeCallback callback);

    private:
        STTHandler(ContextHandle context, std::string vad_model_path, std::optional<NoiseSuppressor> noise_suppressor) noexcept;

        whisper_full_params make_params(size_t num_samples) const;
        float first_sentence_confidence() const;

        ContextHandle context_;
        std::string vad_model_path_;
        std::string initial_prompt_ = CommandParser::whisper_prompt();
        std::optional<NoiseSuppressor> noise_suppressor_;
        int n_threads_;
        std::mutex mutex_;
        boost::asio::thread_pool worker_pool_{1};
    };
}