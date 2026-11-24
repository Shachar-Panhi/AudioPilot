#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <grammar-parser.h>
#include <whisper.h>

namespace CAM::API {

    // A GBNF grammar (see backend/grammars) that steers what Whisper writes while it decodes.
    class CommandGrammar {
    public:
        // reads and parses a .gbnf file
        static std::expected<CommandGrammar, std::string> load(const std::filesystem::path& path);

        // fails on a syntax error or when there is no "root" rule
        static std::expected<CommandGrammar, std::string> parse(std::string_view text);

        ~CommandGrammar() = default;

        CommandGrammar(const CommandGrammar&) = delete;
        CommandGrammar& operator=(const CommandGrammar&) = delete;

        // moving rebuilds rules_, which points into state_
        CommandGrammar(CommandGrammar&& other);
        CommandGrammar& operator=(CommandGrammar&& other);

        // the grammar must outlive the whisper_full call
        void apply(whisper_full_params& params, float penalty) const;

    private:
        explicit CommandGrammar(grammar_parser::parse_state state);

        grammar_parser::parse_state state_;
        size_t root_rule_ = 0;

        // pointers into state_.rules. mutable because whisper_full_params takes a non-const pointer
        mutable std::vector<const whisper_grammar_element*> rules_;
    };

}
