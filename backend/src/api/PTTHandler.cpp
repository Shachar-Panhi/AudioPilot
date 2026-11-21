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

        const size_t reference_size = std::min(recent_audio_.size(), kNoiseReferenceSamples);
        noise_reference_.assign(recent_audio_.end() - static_cast<std::ptrdiff_t>(reference_size), recent_audio_.end());
        recent_audio_.clear();

        active_.store(true, std::memory_order_release);
        spdlog::info("[{}] PTT started", session_id_);
    }

    bool PTTHandler::process_packet(const rtc::binary& packet) {
        const bool active = active_.load(std::memory_order_acquire);

        if (packet.empty() || packet.size() > RtpCpp::kMaxRtpPacketSize) {
            spdlog::warn("[{}] PTT: Invalid RTP packet size ({})", session_id_, packet.size());
            return active;
        }

        std::array<uint8_t, RtpCpp::kMaxRtpPacketSize> buffer{};
        std::memcpy(buffer.data(), packet.data(), packet.size());

        RtpCpp::StaticRtpPacket rtp(buffer);
        if (rtp.parse(packet.size()) != RtpCpp::Result::kSuccess) {
            spdlog::warn("[{}] PTT: Failed to parse RTP packet", session_id_);
            return active;
        }

        const auto& header = rtp.get_header();
        if (header.payload_type_ != PeerConnection::kOpusPayloadType) {
            return false;
        }

        auto payload = rtp.payload();
        if (payload.empty()) {
            spdlog::warn("[{}] PTT: Empty RTP payload", session_id_);
            return active;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        const uint16_t sequence = header.sequence_number_;
        if (last_sequence_) {
            const auto delta = static_cast<uint16_t>(sequence - *last_sequence_);
            if (delta == 0 || delta >= 0x8000U) { //NOLINT
                return active_.load(std::memory_order_relaxed); // duplicate or late packet. its slot was already filled
            }

            const auto lost = static_cast<uint16_t>(delta - 1);
            if (lost <= kMaxConcealedPackets) {
                for (uint16_t i = 0; i < lost; ++i) {
                    decode_and_store({});
                }
            } else {
                // a long gap (e.g. the mic was muted): the decoder's history and the idle audio are stale
                transcoder_.reset_decoder();
                recent_audio_.clear();
            }
        }
        last_sequence_ = sequence;

        decode_and_store(payload);
        return active_.load(std::memory_order_relaxed);
    }

    void PTTHandler::decode_and_store(std::span<const uint8_t> payload) {
        auto decoded_res = transcoder_.decode(payload);
        if (!decoded_res) {
            if (decoded_res.error() != "BUFFERING") {
                spdlog::warn("[{}] PTT: Failed to decode Opus payload: {}", session_id_, decoded_res.error());
            }
            return;
        }

        store_samples(*decoded_res);
    }

    void PTTHandler::store_samples(std::span<const float> samples) {
        if (!active_.load(std::memory_order_relaxed)) {
            // trimmed only once it reaches twice the reference length, so the buffer isn't shifted on every packet
            recent_audio_.insert(recent_audio_.end(), samples.begin(), samples.end());
            if (recent_audio_.size() >= 2 * kNoiseReferenceSamples) {
                recent_audio_.erase(recent_audio_.begin(),
                                    recent_audio_.end() - static_cast<std::ptrdiff_t>(kNoiseReferenceSamples));
            }
            return;
        }

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
        std::vector<float> noise_reference;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            audio_samples.swap(pcm_buffer_);
            noise_reference.swap(noise_reference_);
        }

        const double duration_sec = static_cast<double>(audio_samples.size()) / OpusTranscoder::kSampleRate;
        const double reference_sec = static_cast<double>(noise_reference.size()) / OpusTranscoder::kSampleRate;

        spdlog::info("[{}] PTT stopped - transcribing {:.2f}s of audio ({:.2f}s noise reference)",
                     session_id_, duration_sec, reference_sec);

        if (stt_handler_ && !audio_samples.empty()) {
            stt_handler_->transcribe_async(
                std::move(audio_samples),
                std::move(noise_reference),
                [session_id = session_id_, on_transcript = on_transcript_](std::expected<STTHandler::Transcript, std::string> transcript_res) {
                    if (transcript_res) {
                        spdlog::info("[{}] STT Transcription: \"{}\" (confidence {:.2f})", session_id,
                                     transcript_res->text, transcript_res->confidence);
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