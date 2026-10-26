#pragma once

/**
 * @file known_servers.h
 * @brief The ratcast-servers a client knows of: the build's, and those the
 * servers it met told it of, kept from one run to the next.
 *
 * A server that announces itself knows the others that do and tells each call
 * it helps of them (ratcast/server/server.h); a Call given a data directory
 * keeps what it is told there (kFile), so that the next run starts from more
 * than the build's list without waiting for anything. A server not told of
 * for kForgetAfter is forgotten, the most lately told of kept up to kMost;
 * the build's are never forgotten.
 *
 * The file: a server a line, "HOST:PORT SECONDS", the time it was last told
 * of in seconds since 1970; a line that is not is passed over.
 *
 * Threads: every method may be called from any thread.
 */

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "librats/core/host_endpoint.h"

namespace ratcast {

class KnownServers {
public:
    using Clock = std::chrono::system_clock;

    /// The file's name in a data directory.
    static constexpr std::string_view kFile = "servers";
    static constexpr std::chrono::hours kForgetAfter{24 * 30};
    static constexpr size_t kMost = 64;

    /// The build's list (RATCAST_SERVERS).
    static std::vector<librats::HostEndpoint> built_in();

    /// Kept in `path`, read from it now; "": nowhere.
    explicit KnownServers(std::string path,
                          std::vector<librats::HostEndpoint> build = KnownServers::built_in(),
                          Clock::time_point now = Clock::now());

    /// Every one: the build's first, then those told of, the most lately
    /// first.
    std::vector<librats::HostEndpoint> all() const;

    /// `servers` were told of at `now`: kept, and written out.
    void told(const std::vector<librats::HostEndpoint>& servers,
              Clock::time_point now = Clock::now());

private:
    struct Told {
        librats::HostEndpoint at;
        Clock::time_point when;
    };

    /// Under mutex_: the forgotten dropped, the rest most lately first.
    void tidy(Clock::time_point now);
    void save() const;

    const std::string path_;
    const std::vector<librats::HostEndpoint> built_in_;
    mutable std::mutex mutex_;
    std::vector<Told> told_;
};

}  // namespace ratcast
