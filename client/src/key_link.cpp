#include "key_link.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "ratcast/client/identity.h"

namespace ratcast::key_link {
namespace {

bool starts_with_case(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

std::optional<KeyAndSecret> after(std::string_view rest) {
    // An escaped '.' from a messenger: "%2E".
    std::string text(rest);
    for (size_t at = text.find("%2"); at != std::string::npos; at = text.find("%2", at)) {
        if (at + 2 < text.size() && (text[at + 2] == 'E' || text[at + 2] == 'e')) {
            text.replace(at, 3, ".");
        } else {
            ++at;
        }
    }
    while (!text.empty() && (text.back() == '/' || text.back() == ' ')) text.pop_back();
    const size_t dot = text.find('.');
    if (dot == std::string::npos) return std::nullopt;
    const std::optional<std::vector<uint8_t>> key =
        from_base64url(std::string_view(text).substr(0, dot), 32);
    const std::optional<std::vector<uint8_t>> secret =
        from_base64url(std::string_view(text).substr(dot + 1), 16);
    if (!key || !secret) return std::nullopt;
    KeyAndSecret out;
    std::copy(key->begin(), key->end(), out.key.begin());
    std::copy(secret->begin(), secret->end(), out.secret.begin());
    return out;
}

std::string written(std::span<const uint8_t, 32> key, std::span<const uint8_t, 16> secret) {
    return to_base64url(key) + "." + to_base64url(secret);
}

}  // namespace

std::optional<KeyAndSecret> parse(std::string_view link, std::string_view kind) {
    while (!link.empty() && link.front() == ' ') link.remove_prefix(1);
    const std::string page = "/" + std::string(kind) + "/#";
    for (const std::string_view site : {"https://ratcast.app", "https://www.ratcast.app",
                                        "http://ratcast.app", "http://www.ratcast.app"}) {
        const std::string prefix = std::string(site) + page;
        if (starts_with_case(link, prefix)) return after(link.substr(prefix.size()));
    }
    const std::string scheme = "rats://" + std::string(kind) + "/";
    if (starts_with_case(link, scheme)) return after(link.substr(scheme.size()));
    return std::nullopt;
}

std::string web(std::string_view kind, std::span<const uint8_t, 32> key,
                std::span<const uint8_t, 16> secret) {
    return "https://ratcast.app/" + std::string(kind) + "/#" + written(key, secret);
}

std::string rats(std::string_view kind, std::span<const uint8_t, 32> key,
                 std::span<const uint8_t, 16> secret) {
    return "rats://" + std::string(kind) + "/" + written(key, secret);
}

}  // namespace ratcast::key_link
