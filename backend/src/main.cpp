#include "api/Listener.hpp"
#include "api/coordinator.hpp"
#include "api/PeerConnectionManager.hpp"
#include "api/STTHandler.hpp"

#include <memory>
#include <spdlog/spdlog.h>

int main() {
    constexpr auto kWhisperModelPath = WHISPER_MODEL_PATH;
    constexpr auto kWhisperVadModelPath = WHISPER_VAD_MODEL_PATH;
    constexpr bool kUseGpu = true;

    // runs the Wiener filter noise suppressor on every PTT clip before it reaches Whisper
    constexpr bool kEnableServerNoiseSuppression = true;

    auto stt_result = CAM::API::STTHandler::create(kWhisperModelPath, kWhisperVadModelPath,
                                                   kUseGpu, kEnableServerNoiseSuppression);
    if (!stt_result) {
        spdlog::error("Startup failed: {}", stt_result.error());
        return 1;
    }

    boost::asio::io_context io_context;

    auto ws_manager = std::make_shared<CAM::API::WebsocketManager>();
    auto pc_manager = std::make_shared<CAM::API::PeerConnectionManager>();

    CAM::API::Coordinator coordinator(io_context.get_executor(), ws_manager, pc_manager, std::move(*stt_result));
    auto ws_callback = coordinator.process_ws_callback();
    auto api_callback = coordinator.process_http_callback();

    auto listener = std::make_shared<CAM::API::Listener>(
        io_context.get_executor(),
        std::move(ws_callback),
        std::move(api_callback));

    boost::asio::co_spawn(io_context, listener->listen(), boost::asio::detached);

    io_context.run();
    return 0;
}