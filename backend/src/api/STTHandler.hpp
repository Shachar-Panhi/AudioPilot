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
#include "CommandGrammar.hpp"
#include "CommandParser.hpp"
#include "NoiseSuppressor.hpp"

namespace CAM::API {
    class STTHandler : public std::enable_shared_from_this<STTHandler> {
    public:
        using ContextHandle = std::unique_ptr<whisper_context, decltype(&whisper_free)>;
        struct Transcript {
            std::string text; //NOLINT
            float confidence = 0.0F; //NOLINT  mean token probability of the first sentence
        };
        using TranscribeCallback = std::function<void(std::expected<Transcript, std::string>)>;

        // below this confidence a transcript isn't run as a command (the user is asked to repeat).
        // 0.5 blocks most prompt echoes (~0.5) while keeping real commands (~0.95); higher rejects short commands
        static constexpr float kMinCommandConfidence = 0.5F;

        // the longest command is ~5 tokens. bounds latency if whisper starts repeating itself
        static constexpr int kMaxTokens = 16;

        // VAD settings, far more permissive than whisper's defaults, which dropped real commands
        static constexpr float kVadThreshold = 0.2F;   // counts quiet speech as speech (default 0.5)
        static constexpr int kVadMinSpeechMs = 100;    // keeps short words like "reset" (default 250)
        static constexpr int kVadMinSilenceMs = 500;   // doesn't split a command at pauses (default 100)
        static constexpr int kVadSpeechPadMs = 500;    // keeps the quiet start and end of words (default 30)

        // whisper encodes a 30s window by default. sizing it to the clip is several times faster. the margin and
        // minimum stop whisper from repeating itself, which it did with a tight fit
        static constexpr int kSamplesPerEncoderFrame = 320;
        static constexpr int kAudioCtxMargin = 256;
        static constexpr int kMinAudioCtx = 384;

        // how strongly the command grammar is enforced. 2 only nudges whisper: higher values forced ordinary speech
        // into commands ("Can you hear me now?" -> "AGC 10.")
        static constexpr float kGrammarPenalty = 2.0F;

        static std::expected<std::shared_ptr<STTHandler>, std::string>
        create(const std::filesystem::path& model_path,
               const std::filesystem::path& vad_model_path = {},
               bool use_gpu = true,
               bool enable_noise_suppression = false,
               const std::filesystem::path& grammar_path = {});

        ~STTHandler();

        STTHandler(const STTHandler&) = delete;
        STTHandler& operator=(const STTHandler&) = delete;

        STTHandler(STTHandler&&) = delete;
        STTHandler& operator=(STTHandler&&) = delete;

        // noise_reference is the audio just before the clip. it's used to measure noise, not transcribed
        std::expected<Transcript, std::string>
        transcribe(std::vector<float> pcm_samples, std::span<const float> noise_reference = {});

        void transcribe_async(std::vector<float> pcm_samples, std::vector<float> noise_reference,
                              TranscribeCallback callback);

    private:
        STTHandler(ContextHandle context, std::string vad_model_path, std::optional<NoiseSuppressor> noise_suppressor,
                   std::optional<CommandGrammar> grammar);

        whisper_full_params make_params(size_t num_samples) const;
        float first_sentence_confidence() const;

        ContextHandle context_;
        std::string vad_model_path_;
        std::string initial_prompt_ = CommandParser::whisper_prompt();
        std::optional<NoiseSuppressor> noise_suppressor_;

        std::optional<CommandGrammar> grammar_; // empty when disabled
        int n_threads_;
        std::mutex mutex_;
        boost::asio::thread_pool worker_pool_{1};
    };
}