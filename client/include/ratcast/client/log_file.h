#pragma once

/**
 * @file log_file.h
 * @brief What the process logs, kept in a file as well.
 */

#include <cstddef>
#include <string>

namespace ratcast {

struct LogFileConfig {
    std::string path;
    /// A file grows to this, then the next one is started.
    std::size_t max_size = 5 * 1024 * 1024;
    /// Earlier files kept beside it, `path.1` the newest of them.
    int kept = 4;
};

/// Everything logged — librats', libratcast's, the client's, and whatever is
/// handed to librats' Logger — written to a file too while this lives, the
/// console's output left as it was. Each run starts a file of its own, the
/// previous one moved aside. One at a time: the Logger is the process's.
class LogFile {
public:
    explicit LogFile(const LogFileConfig& config);
    /// The file closed: the console alone again.
    ~LogFile();

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
};

}  // namespace ratcast
