#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace CAM::API {

    // Turns a Whisper transcript into a voice command, written in one canonical form:
    //   "reset", "agc <0-10>", "mixing <object>", "pitch 0", "pitch +<1-12>", "pitch -<1-12>"
    //
    // Whisper writes the same command in different ways ("Pitch minus 6", "Pitch -6", "A G C 7", and "GC 7"
    // when the quiet "A" at the start is lost). The parser accepts those variants, but rejects anything it
    // can't map with confidence instead of guessing.
    class CommandParser {
    public:
        // the vocabulary. the Whisper prompt is built from it, so adding an object here is enough
        static constexpr std::array<std::string_view, 12> kMixingObjects = {
            "helicopter", "fan", "rain", "wind", "traffic", "crowd",
            "ocean", "thunder", "siren", "train", "birds", "engine"};
        static constexpr int kMaxAgc = 10;
        static constexpr int kMaxPitch = 12;

        // the canonical command, or nullopt if the transcript isn't a recognisable command
        static std::optional<std::string> parse(std::string_view transcript);

        // Whisper's initial prompt. whisper reads it as the transcript of audio that came just before, not as
        // instructions, so it copies the prompt's spelling, casing, punctuation and number style. rules:
        //   - one spelling per command, in the exact form it should come out (numbers as digits, signs as words)
        //   - list every mixing object, and spread example numbers across each range
        //   - put the hardest command (AGC) last: the end of the prompt has the most influence
        //   - keep the lead-in sentence. the prompt's first word tokenizes differently ("Reset" -> [Res][et]) from
        //     how whisper writes words ([ Reset], with a leading space), so a command placed first would bias nothing
        //   - only the last 224 tokens are used (this is ~90)
        static std::string whisper_prompt();
    };

}
