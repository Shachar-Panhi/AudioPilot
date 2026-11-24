#include "api/types.hpp"
#include "utils/JsonUtils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using CAM::API::ApiResponsePacket;
using CAM::API::ConnectionPacket;
using CAM::API::HttpCallRequest;
using CAM::API::HttpEndCallRequest;
using CAM::API::HttpPttRequest;
using CAM::API::IceOfferPacket;
using CAM::API::SdpOfferPacket;
using CAM::API::SttResultPacket;
using CAM::Utils::parse_json;
using CAM::Utils::serialize_json;

// field names here are the wire format shared with frontend/src/types/index.ts

TEST_CASE("Server packets serialize with the field names the frontend expects", "[JsonUtils]") {
    SECTION("stt_result") {
        SttResultPacket packet;
        packet.status = "success";
        packet.text = " Pitch minus 6.";
        packet.command = "pitch -6";

        const auto json = serialize_json(packet);
        REQUIRE(json);
        CHECK(*json == R"({"type":"stt_result","status":"success","text":" Pitch minus 6.","command":"pitch -6"})");
    }

    SECTION("api_response") {
        ApiResponsePacket packet;
        packet.action = "disconnect";
        packet.status = "partner_disconnected";

        const auto json = serialize_json(packet);
        REQUIRE(json);
        CHECK(*json == R"({"type":"api_response","action":"disconnect","status":"partner_disconnected"})");
    }

    SECTION("connection") {
        const auto json = serialize_json(ConnectionPacket{.session_id = "abc"});
        REQUIRE(json);
        CHECK(*json == R"({"session_id":"abc"})");
    }
}

TEST_CASE("HTTP request bodies from the frontend parse", "[JsonUtils]") {
    SECTION("/api/call") {
        const auto request = parse_json<HttpCallRequest>(R"({"session_id":"a","target_session_id":"b"})");
        REQUIRE(request);
        CHECK(request->session_id == "a");
        CHECK(request->target_session_id == "b");
    }

    SECTION("/api/end_call") {
        const auto request = parse_json<HttpEndCallRequest>(R"({"session_id":"a"})");
        REQUIRE(request);
        CHECK(request->session_id == "a");
    }

    SECTION("/api/ptt") {
        const auto start = parse_json<HttpPttRequest>(R"({"session_id":"a","active":true})");
        REQUIRE(start);
        CHECK(start->active);

        const auto stop = parse_json<HttpPttRequest>(R"({"session_id":"a","active":false})");
        REQUIRE(stop);
        CHECK_FALSE(stop->active);
    }
}

TEST_CASE("ICE candidates parse with or without sdpMid", "[JsonUtils]") {
    SECTION("with sdpMid") {
        const auto packet = parse_json<IceOfferPacket>(R"({"session_id":"a","candidate":"c","sdpMid":"0"})");
        REQUIRE(packet);
        CHECK(packet->candidate == "c");
        CHECK(packet->sdpMid == "0");
    }

    SECTION("null sdpMid") {
        const auto packet = parse_json<IceOfferPacket>(R"({"session_id":"a","candidate":"c","sdpMid":null})");
        REQUIRE(packet);
        CHECK_FALSE(packet->sdpMid);
    }

    SECTION("missing sdpMid") {
        const auto packet = parse_json<IceOfferPacket>(R"({"session_id":"a","candidate":"c"})");
        REQUIRE(packet);
        CHECK_FALSE(packet->sdpMid);
    }
}

TEST_CASE("SDP and ICE packets can't be mistaken for each other", "[JsonUtils]") {
    // PeerConnection::handle_signaling_message tries SDP first, then ICE, and relies on unknown keys failing
    CHECK_FALSE(parse_json<SdpOfferPacket>(R"({"session_id":"a","candidate":"c","sdpMid":"0"})"));
    CHECK_FALSE(parse_json<IceOfferPacket>(R"({"session_id":"a","sdp":"v=0"})"));
}

TEST_CASE("Malformed JSON is an error", "[JsonUtils]") {
    CHECK_FALSE(parse_json<HttpPttRequest>(""));
    CHECK_FALSE(parse_json<HttpPttRequest>("not json"));
    CHECK_FALSE(parse_json<HttpPttRequest>(R"({"session_id":"a","active":"yes"})"));
}
