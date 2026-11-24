#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace CAM::API {

    // Turns a Whisper transcript into a canonical voice command:
    //   "reset" (spoken "Effect reset"), "agc <0-100>", "mixing <object>", "pitch 0", "pitch +<1-12>", "pitch -<1-12>"
    // Common mishearings are accepted; anything that can't be mapped with confidence is rejected.
    class CommandParser {
    public:
        // the vocabulary. keep in sync with the grammars in backend/grammars
        static constexpr std::array<std::string_view, 12> kMixingObjects = {
            "helicopter", "fan", "rain", "wind", "traffic", "crowd",
            "ocean", "thunder", "siren", "train", "birds", "engine"};
        static constexpr int kMaxAgc = 100;
        static constexpr int kMaxPitch = 12;

        // the canonical command, or nullopt if the transcript isn't a recognisable command
        static std::optional<std::string> parse(std::string_view transcript);

        // Whisper's initial prompt. whisper copies its style rather than following it as instructions, so it lists
        // each command once, in the exact form it should be written. keep the lead-in sentence (the first word
        // tokenizes differently) and AGC last (the end has the most influence). whisper uses the last 224 tokens
        static std::string whisper_prompt();
    };

}
