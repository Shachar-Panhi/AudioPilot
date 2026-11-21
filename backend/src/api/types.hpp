#pragma once

#include <string>
#include <optional>

namespace CAM::API {
    struct ConnectionPacket {
        std::string session_id; //NOLINT
    };

    struct SdpOfferPacket {
        std::string session_id; //NOLINT
        std::string sdp; //NOLINT
    };

    struct IceOfferPacket {
        std::string session_id; //NOLINT
        std::string candidate; //NOLINT
        std::optional<std::string> sdpMid; //NOLINT
    };

    struct HttpCallRequest {
        std::string session_id; //NOLINT
        std::string target_session_id; //NOLINT
    };

    struct HttpEndCallRequest {
        std::string session_id; //NOLINT
    };

    struct HttpPttRequest {
        std::string session_id; //NOLINT
        bool active = false; //NOLINT
    };

    // sent to the client that pressed PTT once its audio has been transcribed
    struct SttResultPacket {
        std::string type = "stt_result"; //NOLINT
        std::string status; //NOLINT  "success" or "error"
        std::string text; //NOLINT    transcript on success, error message otherwise
        std::string command; //NOLINT canonical command ("pitch -6"), empty if the transcript isn't a command
    };

    struct ApiResponsePacket {
        std::string type = "api_response"; //NOLINT
        std::string action; //NOLINT
        std::string status; //NOLINT
    };
}