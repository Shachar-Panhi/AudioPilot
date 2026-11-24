//NOLINTBEGIN

#pragma once

#include <string>
#include <optional>

namespace CAM::API {
    struct ConnectionPacket {
        std::string session_id;
    };

    struct SdpOfferPacket {
        std::string session_id;
        std::string sdp;
    };

    struct IceOfferPacket {
        std::string session_id;
        std::string candidate;
        std::optional<std::string> sdpMid;
    };

    struct HttpCallRequest {
        std::string session_id;
        std::string target_session_id;
    };

    struct HttpEndCallRequest {
        std::string session_id;
    };

    struct HttpPttRequest {
        std::string session_id;
        bool active = false;
    };

    struct SttResultPacket {
        std::string type = "stt_result";
        std::string status;
        std::string text;
        std::string command;
    };

    struct ApiResponsePacket {
        std::string type = "api_response";
        std::string action;
        std::string status;
    };
}

//NOLINTEND