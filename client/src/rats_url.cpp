#include "ratcast/client/rats_url.h"

namespace ratcast {
namespace {

constexpr std::string_view kScheme = "rats://";

}  // namespace

std::optional<RatsUrl> parse_rats_url(std::string_view url) {
    if (!url.starts_with(kScheme)) return std::nullopt;
    url.remove_prefix(kScheme.size());
    if (url.ends_with('/')) url.remove_suffix(1);
    if (auto endpoint = librats::HostEndpoint::parse(url)) return RatsUrl{std::move(*endpoint)};
    if (auto peer = PeerId::from_hex(url)) return RatsUrl{*peer};
    return std::nullopt;
}

std::string to_string(const RatsUrl& url) {
    if (const auto* endpoint = std::get_if<librats::HostEndpoint>(&url)) {
        return std::string(kScheme) + endpoint->to_string();
    }
    return std::string(kScheme) + std::get<PeerId>(url).to_hex();
}

}  // namespace ratcast
