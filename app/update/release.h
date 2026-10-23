#pragma once

/**
 * @file release.h
 * @brief A GitHub release as the updater reads it: its version and the one
 * package in it for this system.
 *
 * The packages are named as .github/workflows/release.yml names them:
 * `ratcast-<version>-windows-x64.zip`, `ratcast-<version>-linux-x86_64.AppImage`,
 * each beside a `-symbols` archive that is never the one.
 */

#include <QByteArray>
#include <QString>
#include <QStringView>
#include <QUrl>
#include <QVersionNumber>

#include <cstdint>
#include <optional>

namespace ratcast::app::update {

struct Release {
    QVersionNumber version;
    /// Its page, for the user who updates by hand.
    QUrl page;
    /// The package: its file name, where it is, how large, and its SHA-256
    /// (empty when GitHub does not say).
    QString asset;
    QUrl url;
    int64_t size = 0;
    QByteArray sha256;
};

/// The release `json` describes (GitHub's `releases/latest`), with its package
/// whose name ends in `suffix`; none for a draft or a prerelease, a tag that
/// is no version, or a release with no such package.
std::optional<Release> parse_release(const QByteArray& json, QStringView suffix);

}  // namespace ratcast::app::update
