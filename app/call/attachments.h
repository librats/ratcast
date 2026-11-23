#pragma once

/**
 * @file attachments.h
 * @brief Files said in a room as the app makes and shows them.
 *
 * A file chosen, dropped or pasted is made a SharedFile off the GUI thread
 * (share_file()): its id read through, and, an image the app reads, its size
 * and a small picture of it for every member to see at once. What is pasted
 * is written as a PNG first (save_pasted()). A file fetched that is no image
 * is put among the user's downloads under a name not taken (free_path()).
 */

#include <QImage>
#include <QString>

#include <atomic>
#include <cstdint>
#include <vector>

#include "ratcast/client/chat_files.h"
#include "ratcast/core/result.h"

namespace ratcast::app {

/// The longest side of an attachment's picture, in pixels: blurred where it
/// is shown larger, as it is until the image is here.
inline constexpr int kPreviewSide = 96;

/// The file at `path` (local) as a message says it: its name, its size and
/// its id, read through — Cancelled once `stop` is set — and, an image the
/// app reads, its size and a small picture of it. Unavailable for a file
/// that cannot be read, or none; InvalidArgument for a name no message
/// carries (is_valid()).
Result<SharedFile> share_file(const QString& path, const std::atomic<bool>* stop = nullptr);

/// `image` small, as a JPEG of at most ChatAttachment::kMaxPreviewSize
/// bytes: none for no image.
std::vector<uint8_t> preview_of(const QImage& image);

/// A picture's bytes as a URL QML shows: data:image/jpeg;base64,…; "" for
/// none.
QString preview_url(const std::vector<uint8_t>& preview);

/// `image` written as a PNG named for when it was pasted, in a new folder
/// of `folder`; "" if it cannot be.
QString save_pasted(const QImage& image, const QString& folder);

/// A path in `folder` for a file named `name` that is not there yet:
/// "name.ext", then "name (1).ext", "name (2).ext", …
QString free_path(const QString& folder, const QString& name);

/// `size` bytes as people read them, in this locale: "1.2 MB".
QString size_text(uint64_t size);

}  // namespace ratcast::app
