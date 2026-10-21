#include "common/arguments.h"

#include <charconv>
#include <utility>

namespace ratcast::tools {

Arguments::Arguments(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--") {
            errors_.push_back("unexpected argument '--'");
            continue;
        }
        if (!arg.starts_with("--")) {
            positional_.emplace_back(arg);
            continue;
        }
        std::string name(arg.substr(2));
        std::optional<std::string> value;
        if (const size_t equals = name.find('='); equals != std::string::npos) {
            value = name.substr(equals + 1);
            name.resize(equals);
        } else if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
            value = argv[++i];
        }
        if (options_.contains(name)) errors_.push_back("--" + name + " given twice");
        options_[name] = Option{std::move(value), false};
    }
}

bool Arguments::flag(std::string_view name) {
    const auto it = options_.find(name);
    if (it == options_.end()) return false;
    it->second.used = true;
    if (it->second.value) positional_.push_back(*std::exchange(it->second.value, std::nullopt));
    return true;
}

std::optional<std::string> Arguments::text(std::string_view name) {
    const auto it = options_.find(name);
    if (it == options_.end()) return std::nullopt;
    it->second.used = true;
    if (!it->second.value) errors_.push_back("--" + it->first + " needs a value");
    return it->second.value;
}

std::optional<uint32_t> Arguments::number(std::string_view name, uint32_t min, uint32_t max) {
    const std::optional<std::string> value = text(name);
    if (!value) return std::nullopt;
    uint32_t number = 0;
    const char* end = value->data() + value->size();
    const auto [stop, error] = std::from_chars(value->data(), end, number);
    if (error != std::errc{} || stop != end || number < min || number > max) {
        errors_.push_back("--" + std::string(name) + " wants a number from " + std::to_string(min) +
                          " to " + std::to_string(max) + ", not '" + *value + "'");
        return std::nullopt;
    }
    return number;
}

std::vector<std::string> Arguments::positional() {
    positional_used_ = true;
    return positional_;
}

std::vector<std::string> Arguments::errors() const {
    std::vector<std::string> errors = errors_;
    if (!positional_used_) {
        for (const std::string& arg : positional_) {
            errors.push_back("unexpected argument '" + arg + "'");
        }
    }
    for (const auto& [name, option] : options_) {
        if (!option.used) errors.push_back("unknown option --" + name);
    }
    return errors;
}

}  // namespace ratcast::tools
