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
        // caps memory and STT cost if PTT is left held down (whisper handles 30s at most anyway)
        static constexpr size_t kMaxPttDurationSec = 30;
        static constexpr size_t kMaxPttSamples = kMaxPttDurationSec * OpusTranscoder::kSampleRate;

        // short packet gaps (up to 100ms) are filled with opus concealment so words aren't glued together.
        // longer ones are left as is rather than inventing a long stretch of audio
        static constexpr uint16_t kMaxConcealedPackets = 5;

        // audio from before the press, used by the noise suppressor to measure the noise. 0.5s gives a stable estimate
        static constexpr size_t kNoiseReferenceSamples = OpusTranscoder::kSampleRate / 2; // 0.5s

        // the recording starts this much before the press, since the "PTT started" request can arrive after the
        // user starts speaking. real recordings showed up to ~0.75s of a command arriving before it
        static constexpr size_t kPreRollSamples = OpusTranscoder::kSampleRate * 3 / 4; // 0.75s

        // called from the STT worker thread with the session id and either the transcript or an error
        using TranscriptCallback =
            std::function<void(const std::string&, std::expected<STTHandler::Transcript, std::string>)>;

        static std::expected<std::unique_ptr<PTTHandler>, std::string>
        create(std::string session_id, std::shared_ptr<STTHandler> stt_handler, TranscriptCallback on_transcript);

        ~PTTHandler() = default;

        PTTHandler(const PTTHandler&) = delete;
        PTTHandler& operator=(const PTTHandler&) = delete;
        PTTHandler(PTTHandler&&) = delete;
        PTTHandler& operator=(PTTHandler&&) = delete;

        void start();
        void stop();

        // decodes every packet, also while idle. returns true if it belongs to a PTT recording, false if it should
        // be forwarded to the call partner
        bool process_packet(const rtc::binary& packet);

    private:
        PTTHandler(std::string session_id, OpusTranscoder transcoder, std::shared_ptr<STTHandler> stt_handler,
                   TranscriptCallback on_transcript) noexcept;

        // both require mutex_ to be held
        void decode_and_store(std::span<const uint8_t> payload);
        void store_samples(std::span<const float> samples);

        std::string session_id_;
        OpusTranscoder transcoder_;
        std::shared_ptr<STTHandler> stt_handler_;
        TranscriptCallback on_transcript_;

        std::mutex mutex_;
        std::atomic<bool> active_{false};
        // idle: the most recent audio. recording: the noise reference (reference_size_ samples), then the command
        std::vector<float> audio_;
        size_t reference_size_ = 0;
        std::optional<uint16_t> last_sequence_;
    };
}