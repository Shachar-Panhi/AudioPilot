#include "api/CommandParser.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using CAM::API::CommandParser;

namespace {
    // the prompt's sentences, minus the lead-in. "A. B. C." -> {"A.", "B.", "C."}
    std::vector<std::string> prompt_sentences() {
        const std::string prompt = CommandParser::whisper_prompt();
        std::vector<std::string> sentences;
        size_t start = 0;
        while (start < prompt.size()) {
            size_t end = prompt.find('.', start);
            end = end == std::string::npos ? prompt.size() : end + 1;
            sentences.emplace_back(" " + prompt.substr(start, end - start));
            start = prompt.find_first_not_of(' ', end);
        }
        sentences.erase(sentences.begin()); // lead-in sentence
        return sentences;
    }
}

TEST_CASE("Strict grammar forms parse to canonical commands", "[CommandParser]") {
    SECTION("reset") {
        CHECK(CommandParser::parse(" Effect reset.") == "reset");
    }

    SECTION("every AGC value in range") {
        for (int value = 0; value <= CommandParser::kMaxAgc; ++value) {
            CHECK(CommandParser::parse(std::format(" AGC {}.", value)) == std::format("agc {}", value));
        }
    }

    SECTION("every pitch value in range") {
        CHECK(CommandParser::parse(" Pitch 0.") == "pitch 0");
        for (int value = 1; value <= CommandParser::kMaxPitch; ++value) {
            CHECK(CommandParser::parse(std::format(" Pitch plus {}.", value)) == std::format("pitch +{}", value));
            CHECK(CommandParser::parse(std::format(" Pitch minus {}.", value)) == std::format("pitch -{}", value));
        }
    }

    SECTION("every mixing object") {
        for (const auto object : CommandParser::kMixingObjects) {
            CHECK(CommandParser::parse(std::format(" Mixing {}.", object)) == std::format("mixing {}", object));
        }
    }
}

TEST_CASE("Common Whisper variants are accepted", "[CommandParser]") {
    const auto [transcript, expected] = GENERATE(table<std::string, std::string>({
        // reset
        {" Reset.", "reset"},
        {" Effects set.", "reset"},
        {" We set.", "reset"},
        {" Effect, re set.", "reset"},
        // AGC: spelled-out letters and a lost leading "A"
        {" A G C 7.", "agc 7"},
        {" A.G.C. 7", "agc 7"},
        {" GC 7.", "agc 7"},
        {" AGC5", "agc 5"},
        {" AGC five.", "agc 5"},
        // pitch: symbols, unicode minus, number words, misheard keyword
        {" Pitch -6.", "pitch -6"},
        {" Pitch +3.", "pitch +3"},
        {" Pitch −6.", "pitch -6"},
        {" Pitch 3.", "pitch +3"},
        {" Pitch minus twelve.", "pitch -12"},
        {" Pitch shift minus 6.", "pitch -6"},
        {" Pitch minus 0.", "pitch 0"},
        {" Fitch minus 6.", "pitch -6"},
        // mixing: split words, small typos, short keyword
        {" Mixing heli copter.", "mixing helicopter"},
        {" Mixing helicoptor.", "mixing helicopter"},
        {" Mix rain.", "mixing rain"},
        // casing and trailing words don't matter
        {" PITCH MINUS 6", "pitch -6"},
        {" Pitch minus 6 please.", "pitch -6"},
    }));

    CAPTURE(transcript);
    CHECK(CommandParser::parse(transcript) == expected);
}

TEST_CASE("The first command in a transcript wins", "[CommandParser]") {
    CHECK(CommandParser::parse(" Mixing rain. Pitch plus 3.") == "mixing rain");
    CHECK(CommandParser::parse(" Pitch plus 3. Mixing rain.") == "pitch +3");
}

TEST_CASE("Out-of-range values are rejected", "[CommandParser]") {
    CHECK_FALSE(CommandParser::parse(std::format(" AGC {}.", CommandParser::kMaxAgc + 1)));
    CHECK_FALSE(CommandParser::parse(std::format(" Pitch plus {}.", CommandParser::kMaxPitch + 1)));
    CHECK_FALSE(CommandParser::parse(std::format(" Pitch minus {}.", CommandParser::kMaxPitch + 1)));
}

TEST_CASE("Incomplete commands are rejected", "[CommandParser]") {
    const auto transcript = GENERATE(as<std::string>{}, " AGC.", " Pitch.", " Pitch minus.", " Mixing.");
    CAPTURE(transcript);
    CHECK_FALSE(CommandParser::parse(transcript));
}

TEST_CASE("Non-commands are rejected", "[CommandParser]") {
    const auto transcript = GENERATE(as<std::string>{},
                               "",
                               " ",
                               " Thank you.",
                               " Hello, how are you?",
                               " Mixing elephant.",
                               " GC.",
                               "...");
    CAPTURE(transcript);
    CHECK_FALSE(CommandParser::parse(transcript));
}

TEST_CASE("The Whisper prompt matches the vocabulary", "[CommandParser]") {
    const std::string prompt = CommandParser::whisper_prompt();

    SECTION("it lists every mixing object") {
        for (const auto object : CommandParser::kMixingObjects) {
            CAPTURE(object);
            CHECK(prompt.contains(std::format("Mixing {}.", object)));
        }
    }

    SECTION("every example sentence is a valid command") {
        // catches examples left out of range after a limit changes
        for (const auto& sentence : prompt_sentences()) {
            CAPTURE(sentence);
            CHECK(CommandParser::parse(sentence).has_value());
        }
    }
}
