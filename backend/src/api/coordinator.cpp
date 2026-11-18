#include "coordinator.hpp"
#include "../utils/Utils.hpp"
#include "../utils/JsonUtils.hpp"
#include "types.hpp"

#include <expected>
#include <string>
#include <utility>

namespace CAM::API {
    namespace {
        // serializes a packet and sends it to a session's websocket, if that session is still connected.
        // must run on the io executor.
        template <typename Packet>
        void send_to_session(const WebsocketManager& ws_manager, const std::string& session_id, const Packet& packet) {
            auto session = ws_manager.get_session(session_id);
            if (!session) {
                return;
            }

            auto json_str = CAM::Utils::serialize_json(packet);
            if (!json_str) {
                spdlog::error("[{}] Failed to serialize packet", session_id);
                return;
            }

            session->dispatch_message(std::move(*json_str));
        }
    }

    Coordinator::Coordinator(boost::asio::any_io_executor executor,
        std::shared_ptr<WebsocketManager> ws_manager,
        std::shared_ptr<PeerConnectionManager> pc_manager, std::shared_ptr<STTHandler> stt_handler)
    : executor_(std::move(executor)), ws_manager_(std::move(ws_manager)), pc_manager_(std::move(pc_manager)),
        bridge_manager_(std::make_shared<BridgeManager>(std::move(stt_handler),
                                                        make_transcript_callback(executor_, ws_manager_))),
        router_(ws_manager_, pc_manager_, bridge_manager_, *this) {}

    PTTHandler::TranscriptCallback Coordinator::make_transcript_callback(boost::asio::any_io_executor executor,
                                                                        std::shared_ptr<WebsocketManager> ws_manager) {
        return [executor = std::move(executor), ws_manager = std::move(ws_manager)](
                   const std::string& session_id, std::expected<std::string, std::string> transcript) {
            SttResultPacket packet;
            packet.status = transcript ? "success" : "error";
            packet.text = transcript ? std::move(*transcript) : std::move(transcript.error());

            boost::asio::post(executor, [ws_manager, session_id, packet = std::move(packet)]() {
                send_to_session(*ws_manager, session_id, packet);
            });
        };
    }

    WsCallback Coordinator::process_ws_callback() {
        return [this](TCP::socket socket, HTTP::request<HTTP::string_body> req) {
            auto executor = socket.get_executor();
            
            auto session_id = CAM::Utils::generate_session_id();
            
            auto on_peer_join = [](const std::shared_ptr<PeerConnection>&) {};
            // fired from a libdatachannel thread, so hop onto the io executor before touching the managers
            auto on_peer_leave = [this, session_id](const std::shared_ptr<PeerConnection>&) {
                boost::asio::post(executor_, [this, session_id]() { disconnect_session(session_id); });
            };

            auto peer_connection = std::make_shared<PeerConnection>(on_peer_join, on_peer_leave, session_id);

            auto on_ws_join = [](const std::shared_ptr<WebsocketSession>&) {};
            auto on_ws_leave = [this, session_id](const std::shared_ptr<WebsocketSession>&) {
                disconnect_session(session_id);
                ws_manager_->remove(session_id);
                pc_manager_->remove(session_id);
            };
            
            auto websocket_session = std::make_shared<WebsocketSession>(std::move(socket), on_ws_join, on_ws_leave, session_id);

            ws_manager_->add(websocket_session, session_id);
            pc_manager_->add(peer_connection, session_id);

            peer_connection->set_signaling_callback([weak_ws = std::weak_ptr<WebsocketSession>(websocket_session)](std::string msg) {
                if (auto websocket = weak_ws.lock()) {
                    websocket->dispatch_message(std::move(msg));
                }
            });

            websocket_session->set_message_callback([peer = peer_connection](const std::string& msg) {
                peer->handle_signaling_message(msg);
            });

            boost::asio::co_spawn(executor, websocket_session->start(std::move(req)), boost::asio::detached);
        };
    }

    ApiCallback Coordinator::process_http_callback() {
        return [this](const HTTP::request<HTTP::string_body>& req) -> HTTP::response<HTTP::string_body> {
            return router_.route(req);
        };
    }

    void Coordinator::disconnect_session(const std::string& session_id) {
        if (pc_manager_->contains(session_id)) {
            pc_manager_->get_peer(session_id)->close();
        }

        if (bridge_manager_->contains(session_id)) {
            std::string partner_id = bridge_manager_->get_partner(session_id);
            bridge_manager_->remove_bridge(session_id);

            if (pc_manager_->contains(partner_id)) {
                pc_manager_->get_peer(partner_id)->close();
            }

            spdlog::info("Call ended between {} and {}", session_id, partner_id);

            ApiResponsePacket partner_res;
            partner_res.action = "disconnect";
            partner_res.status = "partner_disconnected";
            send_to_session(*ws_manager_, partner_id, partner_res);
        }
    }
}