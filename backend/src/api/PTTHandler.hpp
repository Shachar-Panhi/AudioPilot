#pragma once

#include "OpusTranscoder.hpp"
#include "STTHandler.hpp"
#include <rtc/rtc.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace CAM::API {

    class PTTHandler {
    public:
        // whisper processes audio in 30s windows, and voice commands are far shorter.
        // capping the clip bounds memory and the noise suppression / STT cost if PTT is left held down.
        static constexpr size_t kMaxPttDurationSec = 30;
        static constexpr size_t kMaxPttSamples = kMaxPttDurationSec * OpusTranscoder::kSampleRate;

        // up to this many consecutive lost packets (100ms) are filled with opus concealment so words aren't
        // glued together. longer gaps are left as is rather than inventing a long stretch of audio.
        static constexpr uint16_t kMaxConcealedPackets = 5;

        // called from the STT worker thread with the session id and either the transcript or an error
        using TranscriptCallback = std::function<void(const std::string&, std::expected<std::string, std::string>)>;

        static std::expected<std::unique_ptr<PTTHandler>, std::string>
        create(std::string session_id, std::shared_ptr<STTHandler> stt_handler, TranscriptCallback on_transcript);

        ~PTTHandler() = default;

        PTTHandler(const PTTHandler&) = delete;
        PTTHandler& operator=(const PTTHandler&) = delete;
        PTTHandler(PTTHandler&&) = delete;
        PTTHandler& operator=(PTTHandler&&) = delete;

        void start();
        void stop();
        bool process_packet(const rtc::binary& packet);

    private:
        PTTHandler(std::string session_id, OpusTranscoder transcoder, std::shared_ptr<STTHandler> stt_handler,
                   TranscriptCallback on_transcript) noexcept;

        void decode_and_append(std::span<const uint8_t> payload);
        void append_samples(std::span<const float> samples);

        std::string session_id_;
        OpusTranscoder transcoder_;
        std::shared_ptr<STTHandler> stt_handler_;
        TranscriptCallback on_transcript_;

        std::mutex mutex_;
        std::atomic<bool> active_{false};
        std::vector<float> pcm_buffer_;
        std::optional<uint16_t> last_sequence_;
    };
}