#include "CommandGrammar.hpp"

#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace CAM::API {

    CommandGrammar::CommandGrammar(grammar_parser::parse_state state)
        : state_(std::move(state)),
          root_rule_(state_.symbol_ids.at("root")),
          rules_(state_.c_rules()) {}

    CommandGrammar::CommandGrammar(CommandGrammar&& other)
        : state_(std::move(other.state_)),
          root_rule_(other.root_rule_),
          rules_(state_.c_rules()) {}

    CommandGrammar& CommandGrammar::operator=(CommandGrammar&& other) {
        state_ = std::move(other.state_);
        root_rule_ = other.root_rule_;
        rules_ = state_.c_rules();
        return *this;
    }

    std::expected<CommandGrammar, std::string> CommandGrammar::load(const std::filesystem::path& path) {
        std::ifstream file(path);
        if (!file) {
            return std::unexpected(std::format("Command grammar not found: {}", path.string()));
        }
        std::stringstream text;
        text << file.rdbuf();

        auto grammar = parse(text.str());
        if (!grammar) {
            return std::unexpected(std::format("{} ({})", grammar.error(), path.string()));
        }
        return grammar;
    }

    std::expected<CommandGrammar, std::string> CommandGrammar::parse(std::string_view text) {
        // the parser prints syntax errors to stderr and returns an empty grammar
        auto state = grammar_parser::parse(std::string(text).c_str());
        if (state.rules.empty()) {
            return std::unexpected("Invalid command grammar: syntax error");
        }
        if (!state.symbol_ids.contains("root")) {
            return std::unexpected("Invalid command grammar: no 'root' rule");
        }
        return CommandGrammar(std::move(state));
    }

    void CommandGrammar::apply(whisper_full_params& params, float penalty) const {
        params.grammar_rules = rules_.data();
        params.n_grammar_rules = rules_.size();
        params.i_start_rule = root_rule_;
        params.grammar_penalty = penalty;
    }

}
