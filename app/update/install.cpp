#include "install.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStringList>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

Q_LOGGING_CATEGORY(update_log, "ratcast.update")

namespace ratcast::app::update {

namespace {

/// Says what the staged files are: their folder, from staging's.
constexpr char kReady[] = "ready";

fs::path aside(const fs::path& file) {
    fs::path out = file;
    out += kOldSuffix;
    return out;
}

bool is_aside(const fs::path& file) {
    const std::string name = file.filename().string();
    const std::string suffix = kOldSuffix;
    return name.size() > suffix.size() && name.ends_with(suffix);
}

Failure failed(const char* what, const fs::path& path, const std::error_code& error) {
    qCWarning(update_log) << what << to_qstring(path) << ":" << error.message().c_str();
    return fail(Errc::Internal);
}

}  // namespace

std::optional<Target> this_target() {
#if defined(RATCAST_UPDATES) && defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    const fs::path program = to_path(QCoreApplication::applicationFilePath());
    return Target{Target::Kind::Folder, program.parent_path(), program,
                  QStringLiteral("-windows-x64.zip")};
#elif defined(RATCAST_UPDATES) && defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    // Set by the AppImage's runtime: the file, not where it is mounted.
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (appimage.isEmpty()) return std::nullopt;
    const fs::path file = to_path(appimage);
    return Target{Target::Kind::File, file, file, QStringLiteral("-linux-x86_64.AppImage")};
#else
    return std::nullopt;
#endif
}

fs::path to_path(const QString& path) {
#ifdef _WIN32
    return fs::path(path.toStdWString());
#else
    return fs::path(QFile::encodeName(path).toStdString());
#endif
}

QString to_qstring(const fs::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QFile::decodeName(path.c_str());
#endif
}

fs::path download_path(const Target& target, const QString& asset) {
    if (target.kind == Target::Kind::Folder) return staging(target.path) / to_path(asset);
    fs::path out = target.path.parent_path() / ".";
    out += target.path.filename();
    out += ".download";
    return out;
}

fs::path staging(const fs::path& folder) { return folder / ".update"; }

std::optional<fs::path> find_root(const fs::path& extracted, const fs::path& executable) {
    std::error_code error;
    if (fs::is_regular_file(extracted / executable, error)) return extracted;
    std::optional<fs::path> found;
    for (const fs::directory_entry& entry : fs::directory_iterator(extracted, error)) {
        if (!entry.is_directory(error) || !fs::is_regular_file(entry.path() / executable, error)) {
            continue;
        }
        if (found) return std::nullopt;  // which one?
        found = entry.path();
    }
    return found;
}

Result<void> stage(const fs::path& folder, const fs::path& root) {
    const fs::path dir = staging(folder);
    const fs::path ready = dir / kReady;
    fs::path written = ready;
    written += ".part";
    {
        const std::u8string relative = root.lexically_relative(dir).generic_u8string();
        std::ofstream out(written, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(relative.data()), std::streamsize(relative.size()));
        out << '\n';
        if (!out.flush()) {
            return failed("cannot write", written, std::make_error_code(std::errc::io_error));
        }
    }
    std::error_code error;
    fs::rename(written, ready, error);  // all of it or nothing
    if (error) return failed("cannot write", ready, error);
    return {};
}

Result<bool> apply_staged(const fs::path& folder) {
    const fs::path dir = staging(folder);
    std::error_code error;
    if (!fs::exists(dir / kReady, error)) return false;

    std::string relative;
    {
        std::ifstream in(dir / kReady, std::ios::binary);
        std::getline(in, relative);
    }
    const fs::path root =
        (dir / fs::path(std::u8string(relative.begin(), relative.end()))).lexically_normal();
    // Never anything outside staging, nor all of it, whatever the file says.
    const fs::path within = root.lexically_relative(dir);
    const bool inside = !within.empty() && *within.begin() != ".." && within != ".";
    Result<void> replaced =
        inside ? replace_tree(root, folder) : Result<void>(fail(Errc::Malformed));
    fs::remove_all(dir, error);
    if (!replaced) return fail(replaced.error());
    qCInfo(update_log) << "update put in place in" << to_qstring(folder);
    return true;
}

Result<void> replace_tree(const fs::path& from, const fs::path& to, const Rename& rename) {
    const Rename move =
        rename ? rename : [](const fs::path& a, const fs::path& b, std::error_code& error) {
            fs::rename(a, b, error);
        };
    std::error_code error;
    // All of them first: moving files out changes what an iteration would see.
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(from, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error)) files.push_back(it->path().lexically_relative(from));
    }
    if (error) return failed("cannot read", from, error);

    struct Moved {
        fs::path to;
        bool replaced;
    };
    std::vector<Moved> moved;
    const auto undo = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ignored;
            fs::remove(it->to, ignored);
            if (it->replaced) move(aside(it->to), it->to, ignored);
        }
    };

    for (const fs::path& file : files) {
        const fs::path target = to / file;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            undo();
            return failed("cannot make", target.parent_path(), error);
        }
        const bool replaced = fs::exists(target, error);
        if (replaced) {
            std::error_code ignored;
            fs::remove(aside(target), ignored);  // an earlier update's, now free
            move(target, aside(target), error);
            if (error) {
                undo();
                return failed("cannot move aside", target, error);
            }
        }
        move(from / file, target, error);
        if (error) {
            std::error_code ignored;
            if (replaced) move(aside(target), target, ignored);
            undo();
            return failed("cannot put in place", target, error);
        }
        moved.push_back({target, replaced});
    }
    return {};
}

void remove_leftovers(const fs::path& folder) {
    const fs::path skip = staging(folder);
    std::error_code error;
    std::vector<fs::path> old;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end;
         it.increment(error)) {
        if (it->path() == skip) {
            it.disable_recursion_pending();
        } else if (is_aside(it->path())) {
            old.push_back(it->path());
        }
    }
    for (const fs::path& file : old) {
        std::error_code ignored;
        fs::remove(file, ignored);  // held still: the next start's
    }
}

bool start_updated() {
    const std::optional<Target> target = this_target();
    if (!target || target->kind != Target::Kind::Folder) return false;
    remove_leftovers(target->path);
    const Result<bool> applied = apply_staged(target->path);
    if (!applied || !*applied) return false;
    if (!QProcess::startDetached(to_qstring(target->executable),
                                 QCoreApplication::arguments().mid(1))) {
        qCWarning(update_log) << "cannot start the new version: this one goes on";
        return false;
    }
    return true;
}

Result<void> replace_file(const fs::path& downloaded, const fs::path& target) {
    std::error_code error;
    fs::permissions(downloaded,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                    error);
    if (error) return failed("cannot make executable", downloaded, error);
    fs::rename(downloaded, target, error);
    if (error) return failed("cannot put in place", target, error);
    return {};
}

}  // namespace ratcast::app::update
