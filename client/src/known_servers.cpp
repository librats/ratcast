#include "ratcast/client/known_servers.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "ratcast/client/rats_url.h"

namespace ratcast {
namespace {

using Seconds = std::chrono::seconds;

/// "HOST:PORT SECONDS"; nullopt for anything else.
std::optional<std::pair<librats::HostEndpoint, Seconds>> parse_line(std::string_view line) {
    const size_t space = line.rfind(' ');
    if (space == std::string_view::npos) return std::nullopt;
    std::optional<librats::HostEndpoint> at = librats::HostEndpoint::parse(line.substr(0, space));
    const std::string_view number = line.substr(space + 1);
    int64_t seconds = 0;
    const auto [end, error] =
        std::from_chars(number.data(), number.data() + number.size(), seconds);
    if (!at || error != std::errc{} || end != number.data() + number.size()) return std::nullopt;
    return std::pair{std::move(*at), Seconds(seconds)};
}

}  // namespace

std::vector<librats::HostEndpoint> KnownServers::built_in() {
    std::vector<librats::HostEndpoint> servers;
    const std::string_view list = RATCAST_SERVERS;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = std::min(list.find(',', start), list.size());
        if (std::optional<librats::HostEndpoint> at =
                parse_server(list.substr(start, comma - start))) {
            servers.push_back(std::move(*at));
        }
        start = comma + 1;
    }
    return servers;
}

KnownServers::KnownServers(std::string path, std::vector<librats::HostEndpoint> build,
                           Clock::time_point now)
    : path_(std::move(path)), built_in_(std::move(build)) {
    if (path_.empty()) return;
    std::ifstream file(path_);
    for (std::string line; std::getline(file, line);) {
        if (auto parsed = parse_line(line))
            told_.push_back({std::move(parsed->first), Clock::time_point(parsed->second)});
    }
    std::lock_guard lock(mutex_);
    tidy(now);
}

std::vector<librats::HostEndpoint> KnownServers::all() const {
    std::vector<librats::HostEndpoint> servers = built_in_;
    std::lock_guard lock(mutex_);
    for (const Told& told : told_)
        if (std::ranges::find(servers, told.at) == servers.end()) servers.push_back(told.at);
    return servers;
}

void KnownServers::told(const std::vector<librats::HostEndpoint>& servers, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    for (const librats::HostEndpoint& at : servers) {
        const auto known = std::ranges::find(told_, at, &Told::at);
        if (known == told_.end()) {
            told_.push_back({at, now});
        } else {
            known->when = std::max(known->when, now);
        }
    }
    tidy(now);
    save();
}

void KnownServers::tidy(Clock::time_point now) {
    std::erase_if(told_, [&](const Told& told) { return now - told.when > kForgetAfter; });
    std::ranges::stable_sort(told_, std::ranges::greater{}, &Told::when);
    // One told of twice in the file: the later kept.
    std::vector<Told> once;
    for (Told& told : told_)
        if (std::ranges::find(once, told.at, &Told::at) == once.end() && once.size() < kMost)
            once.push_back(std::move(told));
    told_ = std::move(once);
}

void KnownServers::save() const {
    if (path_.empty()) return;
    // Written aside and moved over the old: a run that ends mid-write leaves
    // the old one.
    const std::filesystem::path path(path_);
    std::error_code ignored;  // unkept, the next run starts from the build's
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::filesystem::path aside = path;
    aside += ".new";
    {
        std::ofstream file(aside, std::ios::trunc);
        for (const Told& told : told_) {
            file << told.at.to_string() << ' '
                 << std::chrono::duration_cast<Seconds>(told.when.time_since_epoch()).count()
                 << '\n';
        }
        if (!file.flush()) return;
    }
    std::filesystem::rename(aside, path, ignored);
}

}  // namespace ratcast
