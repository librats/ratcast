#pragma once

/**
 * @file arguments.h
 * @brief The command line of a tool: `--name value`, `--name=value`, `--flag`,
 * and arguments without a name.
 *
 * Each option is asked for by name, with its type; whatever is left unasked at
 * the end is an error the tool reports, so a typo never passes silently. What
 * follows a flag is no value of it but an argument of its own: `--headless
 * rats://…` means what `rats://… --headless` does.
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
    /// Parse `argv[1..argc)`. Options start with "--"; anything else is an
    /// argument without a name.
    Arguments(int argc, const char* const* argv);

    /// Whether `--name` was given (with no value).
    bool flag(std::string_view name);

    /// The value of `--name`, if given.
    std::optional<std::string> text(std::string_view name);
    /// Same, as a number in [min, max]; an error if it is not one.
    std::optional<uint32_t> number(std::string_view name, uint32_t min, uint32_t max);

    /// The arguments without a name, in order. Ask after every flag(): one
    /// may hand back what followed it.
    std::vector<std::string> positional();

    /// What was wrong: unknown or malformed options, values out of range,
    /// arguments without a name nobody asked for. Call after every option has
    /// been asked for.
    std::vector<std::string> errors() const;

private:
    struct Option {
        std::optional<std::string> value;
        bool used = false;
    };

    std::map<std::string, Option, std::less<>> options_;
    std::vector<std::string> positional_;
    bool positional_used_ = false;
    std::vector<std::string> errors_;
};

}  // namespace ratcast::tools
