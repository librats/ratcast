#include "ratcast/client/log_file.h"

#include "librats/util/logger.h"

namespace ratcast {

LogFile::LogFile(const LogFileConfig& config) {
    librats::Logger& logger = librats::Logger::getInstance();
    logger.set_log_rotation_size(config.max_size);
    logger.set_log_retention_count(config.kept);
    logger.set_rotate_on_startup(true);
    logger.set_log_file_path(config.path);
    logger.set_file_logging_enabled(true);
}

LogFile::~LogFile() { librats::Logger::getInstance().set_file_logging_enabled(false); }

}  // namespace ratcast
