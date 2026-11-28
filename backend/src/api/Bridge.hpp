#pragma once

#include "PeerConnection.hpp"
#include "PTTHandler.hpp"
#include "STTHandler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <memory>

namespace CAM::API {
    // one direction of the bridge. the browser keeps counting sequence numbers and timestamps while PTT holds its
    // packets back, so that stretch is cut out of the forwarded stream to keep it continuous for the partner.
    // unsigned arithmetic handles wraparound
    struct RtpRewriteState {
        uint16_t sequence_offset_ = 0;
        uint32_t timestamp_offset_ = 0;
        std::optional<uint32_t> previous_source_timestamp_;

        // records a packet PTT kept from the partner. returns false if it isn't valid RTP (nothing is changed)
        bool suppress(std::span<std::byte> packet);

        // rewrites the sequence number and timestamp in place. returns false if it isn't valid RTP (don't forward)
        bool rewrite(std::span<std::byte> packet);
    };

    class Bridge : public std::enable_shared_from_this<Bridge> {
    public:
        Bridge(std::weak_ptr<PeerConnection> peer1,
               std::weak_ptr<PeerConnection> peer2,
               std::string session1_id,
               std::string session2_id,
               const std::shared_ptr<STTHandler>& stt_handler,
               const PTTHandler::TranscriptCallback& on_transcript);

        bool contains(const std::string& session_id) const;
        std::string get_partner(const std::string& session_id) const;
        void setup_routing();

        const std::string& get_session1() const;
        const std::string& get_session2() const;

        void set_ptt(const std::string& session_id, bool active);

    private:
        void bind_peer_audio(const std::weak_ptr<PeerConnection>& source_peer,
                             const std::weak_ptr<PeerConnection>& target_peer,
                             const std::unique_ptr<PTTHandler>& ptt_handler);

        std::weak_ptr<PeerConnection> peer1_;
        std::weak_ptr<PeerConnection> peer2_;
        std::string session1_id_;
        std::string session2_id_;

        std::unique_ptr<PTTHandler> ptt_handler1_;
        std::unique_ptr<PTTHandler> ptt_handler2_;
    };
}