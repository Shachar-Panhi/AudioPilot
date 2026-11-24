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

    // voice activity detection: skips whisper on clips with no speech
    constexpr bool kUseVad = true;

    // noise suppression on every PTT clip before Whisper
    constexpr bool kEnableServerNoiseSuppression = true;

    // GBNF grammar steering Whisper towards the command formats (backend/grammars). "" turns it off
    constexpr auto kCommandGrammarPath = COMMAND_GRAMMAR_DIR "/commands.gbnf";

    auto stt_result = CAM::API::STTHandler::create(kWhisperModelPath, kUseVad ? kWhisperVadModelPath : "",
                                                   kUseGpu, kEnableServerNoiseSuppression, kCommandGrammarPath);
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