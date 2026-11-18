#include "PTTHandler.hpp"
#include "../ThirdParty/RtpCpp/RtpPacket.hpp"
#include "PeerConnection.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <spdlog/spdlog.h>
#include <utility>

namespace CAM::API {

    PTTHandler::PTTHandler(std::string session_id, OpusTranscoder transcoder, std::shared_ptr<STTHandler> stt_handler,
                           TranscriptCallback on_transcript) noexcept
        : session_id_(std::move(session_id)),
          transcoder_(std::move(transcoder)),
          stt_handler_(std::move(stt_handler)),
          on_transcript_(std::move(on_transcript)) {}

    std::expected<std::unique_ptr<PTTHandler>, std::string>
    PTTHandler::create(std::string session_id, std::shared_ptr<STTHandler> stt_handler, TranscriptCallback on_transcript) {
        auto transcoder = OpusTranscoder::create();
        if (!transcoder) {
            return std::unexpected("PTTHandler: failed to create OpusTranscoder - " + transcoder.error());
        }
        return std::unique_ptr<PTTHandler>(
            new PTTHandler(std::move(session_id), std::move(transcoder.value()), std::move(stt_handler),
                           std::move(on_transcript)));
    }

    void PTTHandler::start() {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm_buffer_.clear();
        last_sequence_.reset();
        transcoder_.reset_decoder();
        active_.store(true, std::memory_order_release);
        spdlog::info("[{}] PTT started", session_id_);
    }

    bool PTTHandler::process_packet(const rtc::binary& packet) {
        if (!active_.load(std::memory_order_acquire)) {
            return false;
        }

        if (packet.empty() || packet.size() > RtpCpp::kMaxRtpPacketSize) {
            spdlog::warn("[{}] PTT: Invalid RTP packet size ({})", session_id_, packet.size());
            return true;
        }

        std::array<uint8_t, RtpCpp::kMaxRtpPacketSize> buffer{};
        std::memcpy(buffer.data(), packet.data(), packet.size());

        RtpCpp::StaticRtpPacket rtp(buffer);
        if (rtp.parse(packet.size()) != RtpCpp::Result::kSuccess) {
            spdlog::warn("[{}] PTT: Failed to parse RTP packet", session_id_);
            return true;
        }

        const auto& header = rtp.get_header();
        if (header.payload_type_ != PeerConnection::kOpusPayloadType) {
            return false;
        }

        auto payload = rtp.payload();
        if (payload.empty()) {
            spdlog::warn("[{}] PTT: Empty RTP payload", session_id_);
            return true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.load(std::memory_order_relaxed)) {
            return false;
        }

        const uint16_t sequence = header.sequence_number_;
        if (last_sequence_) {
            const auto delta = static_cast<uint16_t>(sequence - *last_sequence_);
            if (delta == 0 || delta >= 0x8000U) { //NOLINT
                return true;
            }

            const auto lost = std::min<uint16_t>(delta - 1, kMaxConcealedPackets);
            for (uint16_t i = 0; i < lost; ++i) {
                decode_and_append({});
            }
        }
        last_sequence_ = sequence;

        decode_and_append(payload);
        return true;
    }

    void PTTHandler::decode_and_append(std::span<const uint8_t> payload) {
        auto decoded_res = transcoder_.decode(payload);
        if (!decoded_res) {
            if (decoded_res.error() != "BUFFERING") {
                spdlog::warn("[{}] PTT: Failed to decode Opus payload: {}", session_id_, decoded_res.error());
            }
            return;
        }

        append_samples(*decoded_res);
    }

    void PTTHandler::append_samples(std::span<const float> samples) {
        const size_t remaining = kMaxPttSamples - pcm_buffer_.size();
        if (remaining == 0) {
            return;
        }

        const auto accepted = samples.first(std::min(remaining, samples.size()));
        pcm_buffer_.insert(pcm_buffer_.end(), accepted.begin(), accepted.end());

        if (pcm_buffer_.size() == kMaxPttSamples) {
            spdlog::warn("[{}] PTT: reached the {}s limit, ignoring further audio until PTT stops",
                         session_id_, kMaxPttDurationSec);
        }
    }

    void PTTHandler::stop() {
        std::vector<float> audio_samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            audio_samples.swap(pcm_buffer_);
        }

        const double duration_sec = static_cast<double>(audio_samples.size()) / OpusTranscoder::kSampleRate;

        spdlog::info("[{}] PTT stopped - transcribing {:.2f} seconds of audio with {} samples",
                     session_id_, duration_sec, audio_samples.size());

        if (stt_handler_ && !audio_samples.empty()) {
            stt_handler_->transcribe_async(
                std::move(audio_samples),
                [session_id = session_id_, on_transcript = on_transcript_](std::expected<std::string, std::string> transcript_res) {
                    if (transcript_res) {
                        spdlog::info("[{}] STT Transcription: \"{}\"", session_id, *transcript_res);
                    } else {
                        spdlog::error("[{}] STT Failed: {}", session_id, transcript_res.error());
                    }

                    if (on_transcript) {
                        on_transcript(session_id, std::move(transcript_res));
                    }
                });
        }
    }
}