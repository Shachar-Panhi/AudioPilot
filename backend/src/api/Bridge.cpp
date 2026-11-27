#include "Bridge.hpp"
#include "../ThirdParty/RtpCpp/RtpPacket.hpp"

#include <spdlog/spdlog.h>
#include <string>
#include <utility>

namespace CAM::API {
    namespace {
        // parses without copying. the empty constructor is used because the buffer one writes the version bits
        bool parse_rtp(RtpCpp::RtpPacketView& rtp, std::span<std::byte> packet) {
            // libdatachannel hands over std::byte, RtpCpp reads uint8_t: the same bytes
            auto* data = reinterpret_cast<uint8_t*>(packet.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            return rtp.parse(std::span<uint8_t>(data, packet.size())) == RtpCpp::Result::kSuccess;
        }

        std::unique_ptr<PTTHandler> init_ptt_handler(
            const std::string& session_id,
            const std::shared_ptr<STTHandler>& stt_handler,
            const PTTHandler::TranscriptCallback& on_transcript)
        {
            auto ptt = PTTHandler::create(session_id, stt_handler, on_transcript);
            if (!ptt) {
                spdlog::error("Failed to create PTTHandler for session {}: {}", session_id, ptt.error());
                return nullptr;
            }
            return std::move(ptt.value());
        }
    }

    bool RtpRewriteState::suppress(std::span<std::byte> packet) {
        RtpCpp::RtpPacketView rtp;
        if (!parse_rtp(rtp, packet)) {
            return false;
        }

        const uint32_t timestamp = rtp.get_header().timestamp_;
        ++sequence_offset_;
        // measured from the received timestamps rather than assuming 20ms packets
        if (previous_source_timestamp_) {
            timestamp_offset_ += timestamp - *previous_source_timestamp_;
        }
        previous_source_timestamp_ = timestamp;
        return true;
    }

    bool RtpRewriteState::rewrite(std::span<std::byte> packet) {
        RtpCpp::RtpPacketView rtp;
        if (!parse_rtp(rtp, packet)) {
            return false;
        }

        const auto& header = rtp.get_header();
        const uint32_t timestamp = header.timestamp_;
        rtp.set_sequence_number(static_cast<uint16_t>(header.sequence_number_ - sequence_offset_));
        rtp.set_timestamp(timestamp - timestamp_offset_);
        previous_source_timestamp_ = timestamp;
        return true;
    }

    Bridge::Bridge(std::weak_ptr<PeerConnection> peer1,
                   std::weak_ptr<PeerConnection> peer2,
                   std::string session1_id,
                   std::string session2_id,
                   const std::shared_ptr<STTHandler>& stt_handler,
                   const PTTHandler::TranscriptCallback& on_transcript)
        : peer1_(std::move(peer1)),
          peer2_(std::move(peer2)),
          session1_id_(std::move(session1_id)),
          session2_id_(std::move(session2_id)),
          ptt_handler1_(init_ptt_handler(session1_id_, stt_handler, on_transcript)),
          ptt_handler2_(init_ptt_handler(session2_id_, stt_handler, on_transcript)) {}

    bool Bridge::contains(const std::string& session_id) const {
        return session1_id_ == session_id || session2_id_ == session_id;
    }

    std::string Bridge::get_partner(const std::string& session_id) const {
        if (session1_id_ == session_id) {
            return session2_id_;
        }
        if (session2_id_ == session_id) {
            return session1_id_;
        }
        return "";
    }

    void Bridge::setup_routing() {
        bind_peer_audio(peer1_, peer2_, ptt_handler1_);
        bind_peer_audio(peer2_, peer1_, ptt_handler2_);
    }

    void Bridge::bind_peer_audio(const std::weak_ptr<PeerConnection>& source_peer,
                                 const std::weak_ptr<PeerConnection>& target_peer,
                                 const std::unique_ptr<PTTHandler>& ptt_handler) {
        auto source = source_peer.lock();
        if (!source) {
            return;
        }

        PTTHandler* handler_ptr = ptt_handler.get();
        // each direction gets its own lambda, so each gets its own rewrite state
        source->set_audio_callback([weak_self = weak_from_this(), target_peer, handler_ptr,
                                    rewrite_state = RtpRewriteState{}](const rtc::binary& packet) mutable {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }

            rtc::binary forwarded = packet;
            if (handler_ptr != nullptr && handler_ptr->process_packet(packet)) {
                rewrite_state.suppress(forwarded);
                return;
            }

            if (!rewrite_state.rewrite(forwarded)) {
                return;
            }

            if (auto target = target_peer.lock()) {
                target->send_audio_packet(forwarded);
            }
        });
    }

    void Bridge::set_ptt(const std::string& session_id, bool active) {
        const bool is_peer1 = (session_id == session1_id_);
        const bool is_peer2 = (session_id == session2_id_);
        if (!is_peer1 && !is_peer2) {
            return;
        }

        auto& handler = is_peer1 ? ptt_handler1_ : ptt_handler2_;
        if (!handler) {
            return;
        }

        if (active) {
            handler->start();
        } else {
            handler->stop();
        }
    }

    const std::string& Bridge::get_session1() const {
        return session1_id_;
    }

    const std::string& Bridge::get_session2() const {
        return session2_id_;
    }
}