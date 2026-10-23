#pragma once

/**
 * @file install.h
 * @brief Putting a downloaded release in place of the one running.
 *
 * On Linux the app is one file, an AppImage: the new one is renamed over it,
 * which the one running (mounted from the old file, which lives on while it is
 * open) never notices; it starts with the next launch.
 *
 * On Windows the app is a folder, and files in use can be renamed but not
 * overwritten. The release is unpacked into the folder's `.update` and staged
 * there; the next start, before anything else, moves each of its files in
 * place of the old one — renamed aside with kOldSuffix, each move undone if
 * one fails — and starts the new program in its own place. What is left aside
 * is removed by a later start, once no process holds it.
 *
 * Plain functions over std::filesystem, which Updater drives; tests run them
 * on folders of their own.
 */

#include <QLoggingCategory>
#include <QString>

#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>

#include "ratcast/core/result.h"

Q_DECLARE_LOGGING_CATEGORY(update_log)

namespace ratcast::app::update {

namespace fs = std::filesystem;

/// What follows an old file renamed aside.
inline constexpr char kOldSuffix[] = ".ratcast-old";

/// What this process updates, and how.
struct Target {
    enum class Kind {
        Folder,  ///< the folder the Windows zip unpacks to
        File,    ///< the AppImage
    };
    Kind kind = Kind::Folder;
    fs::path path;        ///< the folder, or the file
    fs::path executable;  ///< what starts the app again
    QString suffix;       ///< of its package's name (release.h)
};

/// This process's, or none: a build that is no release package
/// (RATCAST_UPDATES), or one that cannot replace itself — not started from an
/// AppImage on Linux; macOS, which has no package yet.
std::optional<Target> this_target();

fs::path to_path(const QString& path);
QString to_qstring(const fs::path& path);

/// Where a download goes before it is put in place: beside the AppImage, or
/// in the folder's staging, so that renaming it in place never crosses a disk.
fs::path download_path(const Target& target, const QString& asset);

// ── A folder ────────────────────────────────────────────────────────────────

/// Where an update of `folder` is unpacked and waits for the next start.
fs::path staging(const fs::path& folder);

/// Of what `extracted` holds, the folder with `executable` in it: itself, or
/// the one folder the archive put it in.
std::optional<fs::path> find_root(const fs::path& extracted, const fs::path& executable);

/// Says the files under `root` (in staging(folder)) are to replace the
/// folder's at the next start. The last step of a download.
Result<void> stage(const fs::path& folder, const fs::path& root);

/// At start: the staged update put in place; true if there was one and it
/// went in, the new program then to start in this one's place. Staging is
/// gone after it, whatever happened: a failed update is downloaded again.
Result<bool> apply_staged(const fs::path& folder);

using Rename = std::function<void(const fs::path& from, const fs::path& to, std::error_code&)>;

/// Every file under `from` moved to the same place under `to`, the one it
/// replaces renamed aside with kOldSuffix; if one cannot be, all are put back
/// as they were. `rename` is std::filesystem's but in tests.
Result<void> replace_tree(const fs::path& from, const fs::path& to, const Rename& rename = {});

/// What earlier updates left aside under `folder`, removed where nothing
/// holds it any more.
void remove_leftovers(const fs::path& folder);

/// The first thing the app does: an update staged by an earlier run put in
/// place, and the new program started with this one's arguments; true then,
/// and this one is to quit at once.
bool start_updated();

// ── A file ──────────────────────────────────────────────────────────────────

/// `downloaded` made executable and renamed over `target`.
Result<void> replace_file(const fs::path& downloaded, const fs::path& target);

}  // namespace ratcast::app::update
