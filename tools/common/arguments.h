#pragma once

/**
 * @file arguments.h
 * @brief The command line of a tool: `--name value`, `--name=value`, `--flag`.
 *
 * Each option is asked for by name, with its type; whatever is left unasked at
 * the end is an error the tool reports, so a typo never passes silently.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ratcast::tools {

class Arguments {
public:
    /// Parse `argv[1..argc)`. Options must start with "--"; anything else is
    /// an error, reported by errors().
    Arguments(int argc, const char* const* argv);

    /// Whether `--name` was given (with no value).
    bool flag(std::string_view name);

    /// The value of `--name`, if given.
    std::optional<std::string> text(std::string_view name);
    /// Same, as a number in [min, max]; an error if it is not one.
    std::optional<uint32_t> number(std::string_view name, uint32_t min, uint32_t max);

    /// What was wrong: unknown or malformed options, values out of range.
    /// Call after every option has been asked for.
    std::vector<std::string> errors() const;

private:
    struct Option {
        std::optional<std::string> value;
        bool used = false;
    };

    std::map<std::string, Option, std::less<>> options_;
    std::vector<std::string> errors_;
};

}  // namespace ratcast::tools
