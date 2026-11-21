#include "STTHandler.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <spdlog/spdlog.h>
#include <thread>
#include <utility>

namespace CAM::API {

    STTHandler::STTHandler(ContextHandle context, std::string vad_model_path, std::optional<NoiseSuppressor> noise_suppressor) noexcept
        : context_(std::move(context)),
          vad_model_path_(std::move(vad_model_path)),
          noise_suppressor_(std::move(noise_suppressor)),
          n_threads_(std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4)) {}

    STTHandler::~STTHandler() {
        worker_pool_.join();
    }

    std::expected<std::shared_ptr<STTHandler>, std::string>
    STTHandler::create(const std::filesystem::path& model_path,
                       const std::filesystem::path& vad_model_path,
                       bool use_gpu,
                       bool enable_noise_suppression) {
        if (!std::filesystem::exists(model_path)) {
            return std::unexpected(
                std::format("Whisper model file not found: {}", model_path.string()));
        }

        whisper_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);

        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = use_gpu;
        cparams.flash_attn = true;

        const std::string path_str = model_path.string();
        ContextHandle ctx(
            whisper_init_from_file_with_params(path_str.c_str(), cparams),
            whisper_free);

        if (!ctx) {
            return std::unexpected(
                std::format("Failed to initialize Whisper context from: {}", path_str));
        }

        std::optional<NoiseSuppressor> noise_suppressor;
        if (enable_noise_suppression) {
            auto created = NoiseSuppressor::create();
            if (!created) {
                return std::unexpected(created.error());
            }
            noise_suppressor = std::move(*created);
        }

        std::string valid_vad_path;
        if (!vad_model_path.empty() && std::filesystem::exists(vad_model_path)) {
            valid_vad_path = vad_model_path.string();
            spdlog::info("Whisper VAD model enabled: {}", valid_vad_path);
        }

        spdlog::info("Whisper STT initialized ({}, server noise suppression {}). System optimizations: {}",
                     model_path.filename().string(),
                     noise_suppressor ? "on" : "off",
                     whisper_print_system_info());

        return std::shared_ptr<STTHandler>(
            new STTHandler(std::move(ctx), std::move(valid_vad_path), std::move(noise_suppressor)));
    }

    std::expected<STTHandler::Transcript, std::string>
    STTHandler::transcribe(std::vector<float> pcm_samples, std::span<const float> noise_reference) {
        if (pcm_samples.empty()) {
            return Transcript{};
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (noise_suppressor_) {
            const auto denoise_start = std::chrono::steady_clock::now();
            noise_suppressor_->process(pcm_samples, noise_reference);
            spdlog::debug("Noise suppression: {} samples in {}us", pcm_samples.size(),
                          std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - denoise_start).count());
        }

        const auto stt_start = std::chrono::steady_clock::now();
        const int ret = whisper_full(
            context_.get(),
            make_params(pcm_samples.size()),
            pcm_samples.data(),
            static_cast<int>(pcm_samples.size()));
        spdlog::debug("Whisper: {} samples in {}ms", pcm_samples.size(),
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - stt_start).count());

        if (ret != 0) {
            return std::unexpected(std::format("whisper_full failed with code {}", ret));
        }

        const int n_segments = whisper_full_n_segments(context_.get());
        std::string transcript;

        for (int i = 0; i < n_segments; ++i) {
            const char* segment_text = whisper_full_get_segment_text(context_.get(), i);
            if (segment_text != nullptr) {
                transcript += segment_text;
            }
        }

        const auto first = transcript.find_first_not_of(" \t\n");
        if (first == std::string::npos) {
            return Transcript{};
        }
        const auto last = transcript.find_last_not_of(" \t\n");
        return Transcript{.text = transcript.substr(first, last - first + 1), .confidence = first_sentence_confidence()};
    }

    float STTHandler::first_sentence_confidence() const {
        if (whisper_full_n_segments(context_.get()) == 0) {
            return 0.0F;
        }

        double total = 0.0;
        int count = 0;
        const whisper_token end_of_text = whisper_token_eot(context_.get());
        for (int i = 0; i < whisper_full_n_tokens(context_.get(), 0); ++i) {
            const whisper_token_data token = whisper_full_get_token_data(context_.get(), 0, i);
            if (token.id >= end_of_text) {
                continue; // special tokens (start of transcript, language, ...) aren't part of the text
            }
            total += token.p;
            ++count;

            // stop at the end of the first sentence: whisper sometimes keeps repeating the command after it
            if (std::string_view(whisper_full_get_token_text(context_.get(), 0, i)).contains('.')) {
                break;
            }
        }
        return count == 0 ? 0.0F : static_cast<float>(total / count);
    }

    whisper_full_params STTHandler::make_params(size_t num_samples) const {
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.n_threads = n_threads_;
        wparams.language = "en";
        wparams.translate = false;
        wparams.no_context = true;
        wparams.single_segment = true;
        wparams.no_timestamps = true;
        wparams.max_tokens = kMaxTokens;
        wparams.suppress_blank = true;
        wparams.suppress_nst = true;
        wparams.no_speech_thold = 0.6F; //NOLINT
        wparams.print_progress = false;
        wparams.print_realtime = false;
        wparams.print_special = false;
        wparams.print_timestamps = false;
        wparams.initial_prompt = initial_prompt_.c_str();

        // no retries at higher temperature. they were triggered by the command-list prompt, quadrupled latency
        // (~350ms -> ~1.4s) and made results vary from clip to clip. with the parser, one greedy pass is enough
        wparams.temperature_inc = 0.0F;

        // raw clip length is an upper bound on what's left after VAD trims silence
        const auto clip_frames = static_cast<int>((num_samples + kSamplesPerEncoderFrame - 1) / kSamplesPerEncoderFrame);
        wparams.audio_ctx = std::clamp(clip_frames + kAudioCtxMargin, kMinAudioCtx,
                                       whisper_model_n_audio_ctx(context_.get()));

        if (!vad_model_path_.empty()) {
            wparams.vad = true;
            wparams.vad_model_path = vad_model_path_.c_str();
            wparams.vad_params = whisper_vad_default_params();
            wparams.vad_params.speech_pad_ms = kVadSpeechPadMs;
        }

        return wparams;
    }

    void STTHandler::transcribe_async(std::vector<float> pcm_samples, std::vector<float> noise_reference,
                                      TranscribeCallback callback) {
        boost::asio::post(worker_pool_, [self = shared_from_this(),
                                         samples = std::move(pcm_samples),
                                         reference = std::move(noise_reference),
                                         callback = std::move(callback)]() mutable {
            auto result = self->transcribe(std::move(samples), reference);
            if (callback) {
                callback(std::move(result));
            }
        });
    }

}