#include "api/CommandGrammar.hpp"
#include "api/CommandParser.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using CAM::API::CommandGrammar;
using CAM::API::CommandParser;

namespace {
    const std::filesystem::path kGrammarDir = COMMAND_GRAMMAR_DIR;

    std::string read_file(const std::filesystem::path& path) {
        const std::ifstream file(path);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    // the quoted words of the "object ::=" rule, which may continue over lines ending in '|'
    std::set<std::string> grammar_objects(const std::string& grammar) {
        std::set<std::string> objects;
        std::istringstream lines(grammar);
        std::string line;
        bool in_rule = false;
        while (std::getline(lines, line)) {
            if (line.starts_with("object ")) {
                in_rule = true;
            }
            if (!in_rule) {
                continue;
            }
            for (size_t open = line.find('"'); open != std::string::npos; open = line.find('"', open + 1)) {
                const size_t close = line.find('"', open + 1);
                objects.insert(line.substr(open + 1, close - open - 1));
                open = close;
            }
            if (!line.ends_with('|')) {
                break;
            }
        }
        return objects;
    }

    whisper_full_params default_params() { return whisper_full_default_params(WHISPER_SAMPLING_GREEDY); }
}

TEST_CASE("A valid grammar parses", "[CommandGrammar]") {
    CHECK(CommandGrammar::parse(R"(root ::= " " "Reset")").has_value());
}

TEST_CASE("Invalid grammars are rejected", "[CommandGrammar]") {
    SECTION("syntax error") {
        const auto grammar = CommandGrammar::parse(R"(root ::= "unterminated)");
        REQUIRE_FALSE(grammar);
        CHECK(grammar.error().contains("syntax error"));
    }

    SECTION("empty text") {
        CHECK_FALSE(CommandGrammar::parse(""));
    }

    SECTION("no root rule") {
        const auto grammar = CommandGrammar::parse(R"(command ::= "Reset")");
        REQUIRE_FALSE(grammar);
        CHECK(grammar.error().contains("root"));
    }
}

TEST_CASE("Loading a missing file fails with its path", "[CommandGrammar]") {
    const auto path = kGrammarDir / "does-not-exist.gbnf";
    const auto grammar = CommandGrammar::load(path);
    REQUIRE_FALSE(grammar);
    CHECK(grammar.error().contains(path.string()));
}

TEST_CASE("The project's grammars load", "[CommandGrammar]") {
    const auto file = GENERATE(as<std::string>{}, "commands.gbnf", "commands-flexible.gbnf");
    CAPTURE(file);
    const auto grammar = CommandGrammar::load(kGrammarDir / file);
    CHECK(grammar.has_value());
}

TEST_CASE("The grammars list exactly the parser's mixing objects", "[CommandGrammar]") {
    const auto file = GENERATE(as<std::string>{}, "commands.gbnf", "commands-flexible.gbnf");
    CAPTURE(file);

    const std::set<std::string> expected(CommandParser::kMixingObjects.begin(), CommandParser::kMixingObjects.end());
    CHECK(grammar_objects(read_file(kGrammarDir / file)) == expected);
}

TEST_CASE("apply points whisper at the root rule", "[CommandGrammar]") {
    // root isn't the first rule, so this checks the start rule index, not just the rule list
    const auto grammar = CommandGrammar::parse("other ::= \"b\"\nroot ::= \"a\"");
    REQUIRE(grammar);

    auto params = default_params();
    constexpr float kPenalty = 42.0F;
    grammar->apply(params, kPenalty);

    REQUIRE(params.grammar_rules != nullptr);
    CHECK(params.n_grammar_rules == 2);
    CHECK(params.grammar_penalty == kPenalty);

    const whisper_grammar_element& first = params.grammar_rules[params.i_start_rule][0];
    CHECK(first.type == WHISPER_GRETYPE_CHAR);
    CHECK(first.value == 'a');
}

TEST_CASE("A moved grammar still applies correctly", "[CommandGrammar]") {
    auto parsed = CommandGrammar::parse(R"(root ::= "a")");
    REQUIRE(parsed);

    SECTION("move construction") {
        const CommandGrammar moved(std::move(*parsed));
        auto params = default_params();
        moved.apply(params, 1.0F);
        CHECK(params.grammar_rules[params.i_start_rule][0].value == 'a');
    }

    SECTION("move assignment") {
        auto other = CommandGrammar::parse(R"(root ::= "b")");
        REQUIRE(other);
        *other = std::move(*parsed);
        auto params = default_params();
        other->apply(params, 1.0F);
        CHECK(params.grammar_rules[params.i_start_rule][0].value == 'a');
    }
}
