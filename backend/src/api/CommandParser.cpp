#include "CommandParser.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <span>
#include <vector>

namespace CAM::API {

    namespace {

        bool is_letter(char letter) { return std::isalpha(static_cast<unsigned char>(letter)) != 0; }
        bool is_digit(char digit) { return std::isdigit(static_cast<unsigned char>(digit)) != 0; }
        bool is_word(std::string_view word) { return !word.empty() && std::ranges::all_of(word, is_letter); }

        int edit_distance(std::string_view view_a, std::string_view view_b) {
            std::vector<int> previous(view_b.size() + 1);
            std::vector<int> current(view_b.size() + 1);
            for (size_t j = 0; j <= view_b.size(); ++j) {
                previous[j] = static_cast<int>(j);
            }
            for (size_t i = 1; i <= view_a.size(); ++i) {
                current[0] = static_cast<int>(i);
                for (size_t j = 1; j <= view_b.size(); ++j) {
                    const int substitution = previous[j - 1] + (view_a[i - 1] != view_b[j - 1] ? 1 : 0);
                    current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
                }
                std::swap(previous, current);
            }
            return previous[view_b.size()];
        }

        // lowercase words and numbers, with "+" and "-" as separate tokens. "gc5" -> "gc 5", "A.G.C." -> "a g c"
        std::vector<std::string> split_words(std::string_view text) {
            std::vector<std::string> words;
            std::string word;
            const auto flush = [&] {
                if (!word.empty()) {
                    words.push_back(std::move(word));
                    word.clear();
                }
            };

            for (size_t i = 0; i < text.size(); ++i) {
                const char letter = text[i];
                if (is_letter(letter) || is_digit(letter)) {
                    if (!word.empty() && is_digit(word.back()) != is_digit(letter)) {
                        flush();
                    }
                    word += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
                } else if (letter == '+' || letter == '-') {
                    flush();
                    words.emplace_back(1, letter);
                } else if (text.substr(i).starts_with("−")) { // unicode minus sign
                    flush();
                    words.emplace_back("-");
                    i += std::string_view("−").size() - 1;
                } else {
                    flush();
                }
            }
            flush();
            return words;
        }

        // joins spelled-out letters back into one word: "a g c" -> "agc", and "a gc" / "ag c" -> "agc"
        std::vector<std::string> join_letters(const std::vector<std::string>& words) {
            std::vector<std::string> joined;
            for (size_t i = 0; i < words.size(); ++i) {
                std::string current = words[i];
                if (current.size() == 1 && is_word(current)) {
                    while (i + 1 < words.size() && words[i + 1].size() == 1 && is_word(words[i + 1])) {
                        current += words[++i];
                    }
                }
                joined.push_back(std::move(current));
            }

            std::vector<std::string> merged;
            for (size_t i = 0; i < joined.size(); ++i) {
                const bool fragment = joined[i].size() <= 2 && is_word(joined[i]);
                if (fragment && i + 1 < joined.size() && joined[i + 1].size() <= 2 && is_word(joined[i + 1]) &&
                    joined[i].size() + joined[i + 1].size() == 3) {
                    merged.push_back(joined[i] + joined[i + 1]);
                    ++i;
                } else {
                    merged.push_back(joined[i]);
                }
            }
            return merged;
        }

        // digits, number words, and words Whisper writes instead of a spoken number ("to" -> 2, "for" -> 4)
        std::optional<int> parse_number(std::string_view word) {
            static constexpr std::array<std::pair<std::string_view, int>, 25> kWords = {{
                {"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6},
                {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12},
                {"oh", 0}, {"won", 1}, {"to", 2}, {"too", 2}, {"for", 4}, {"fore", 4}, {"sex", 6},
                {"ate", 8}, {"tree", 3}, {"free", 3}, {"tin", 10}, {"then", 10}}};

            if (!word.empty() && word.size() <= 3 && std::ranges::all_of(word, is_digit)) {
                return std::stoi(std::string(word));
            }
            const auto* found = std::ranges::find(kWords, word, &std::pair<std::string_view, int>::first);
            if (found != kWords.end()) {
                return found->second;
            }
            return std::nullopt;
        }

        // exact match, or close enough to be a mishearing ("fitch" -> "pitch", "mixin" -> "mixing")
        bool matches_keyword(std::string_view word, std::string_view keyword, int max_distance) {
            if (word == keyword) {
                return true;
            }
            if (word.size() < 2) {
                return false;
            }
            const int distance = edit_distance(word, keyword);
            return (word.front() == keyword.front() && distance <= max_distance) || (keyword.size() >= 5 && distance <= 1); //NOLINT
        }

        bool is_one_of(std::string_view word, std::initializer_list<std::string_view> options) {
            return std::ranges::find(options, word) != options.end();
        }

        std::optional<std::string> parse_agc(std::span<const std::string> args) {
            if (args.empty()) {
                return std::nullopt;
            }
            const auto value = parse_number(args.front());
            if (!value || *value > CommandParser::kMaxAgc) {
                return std::nullopt;
            }
            return std::format("agc {}", *value);
        }

        std::optional<std::string> parse_pitch(std::span<const std::string> args) {
            int sign = 1;
            if (!args.empty() && is_one_of(args.front(), {"-", "minus", "negative", "minis", "minas", "mine"})) {
                sign = -1;
                args = args.subspan(1);
            } else if (!args.empty() && is_one_of(args.front(), {"+", "plus", "positive", "plush", "please"})) {
                args = args.subspan(1);
            }

            if (args.empty()) {
                return std::nullopt;
            }
            const auto value = parse_number(args.front());
            if (!value || *value > CommandParser::kMaxPitch) {
                return std::nullopt;
            }
            if (*value == 0) {
                return "pitch 0";
            }
            return std::format("pitch {}{}", sign < 0 ? '-' : '+', *value);
        }

        // the closest object, if it's close enough to be a mishearing. "ring" is as close to "rain" as to "wind",
        // so it's rejected rather than guessed
        std::optional<std::string> parse_mixing(std::span<const std::string> args) {
            if (args.empty()) {
                return std::nullopt;
            }
            std::string two_words = args.front();
            if (args.size() > 1) {
                two_words += args[1]; // "heli copter" -> "helicopter"
            }

            int best_distance = 1000; //NOLINT
            std::string_view best;
            for (const auto object : CommandParser::kMixingObjects) {
                for (const auto& candidate : {args.front(), two_words}) {
                    const int distance = edit_distance(candidate, object);
                    if (distance < best_distance) {
                        best_distance = distance;
                        best = object;
                    }
                }
            }

            if (best_distance > std::max(1, static_cast<int>(best.size()) / 3)) {
                return std::nullopt;
            }
            return std::format("mixing {}", best);
        }

    }

    std::optional<std::string> CommandParser::parse(std::string_view transcript) {
        const auto words = join_letters(split_words(transcript));
        const auto all = std::span(words);

        // the first command keyword decides the command. anything after the command is ignored
        for (size_t i = 0; i < words.size(); ++i) {
            const auto& word = words[i];
            const auto args = all.subspan(i + 1);

            if (matches_keyword(word, "reset", 1)) {
                return "reset";
            }
            // "We set" / "We said": "reset" with the R heard as a W
            if (is_one_of(word, {"re", "we", "wee"}) && !args.empty() &&
                is_one_of(args.front(), {"set", "said", "send", "sent", "sat"})) {
                return "reset";
            }
            if (is_one_of(word, {"agc", "agec", "egc", "ajc", "abc", "agg", "aegc"})) {
                return parse_agc(args);
            }
            if (word == "age" && !args.empty() && args.front() == "c") {
                return parse_agc(args.subspan(1));
            }
            // "GC 6", "DC 9", "ACC 6": a mishearing of "A G C" (often the quiet "A" at the start is lost).
            // only accepted when a valid AGC value follows
            const bool agc_like = edit_distance(word, "gc") <= 1 || edit_distance(word, "agc") <= 1;
            if (word.size() >= 2 && word.size() <= 4 && is_word(word) && agc_like && !args.empty()) {
                if (auto agc = parse_agc(args)) {
                    return agc;
                }
            }
            if (matches_keyword(word, "pitch", 1) || is_one_of(word, {"pitched", "peach", "pinch"})) {
                return parse_pitch(args);
            }
            if (matches_keyword(word, "mixing", 2) || is_one_of(word, {"mix", "mixin", "mixed", "mixer", "mixes"})) {
                return parse_mixing(args);
            }
        }
        return std::nullopt;
    }

    std::string CommandParser::whisper_prompt() {
        std::string prompt = "Audio console voice commands. Reset. ";
        for (const auto object : kMixingObjects) {
            prompt += std::format("Mixing {}. ", object);
        }
        // signs as words: whisper otherwise sometimes dropped the "minus" ("Pitch minus 1" became "Pitch 1")
        prompt += "Pitch 0. Pitch plus 3. Pitch minus 6. Pitch plus 12. Pitch minus 1. ";
        // AGC goes last. it's the hardest command to hear ("A G C" is three short letters), and the end of the prompt
        // influences whisper the most. placed earlier, noisy AGC commands were often heard as "Pitch 0."
        prompt += "AGC 0. AGC 3. AGC 5. AGC 8. AGC 10.";
        return prompt;
    }

}
