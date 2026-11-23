#include "ratcast/client/chat_files.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

#include "monocypher.h"

namespace ratcast {

Result<FileId> file_id(const std::string& path, const std::atomic<bool>* stop) {
    std::ifstream in(std::filesystem::path(
                         std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                     std::ios::binary);
    if (!in) return fail(Errc::Unavailable);
    crypto_blake2b_ctx hash;
    crypto_blake2b_init(&hash, FileId{}.size());
    std::vector<char> block(size_t{1} << 20);
    while (in) {
        if (stop && stop->load(std::memory_order_relaxed)) return fail(Errc::Cancelled);
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const std::streamsize read = in.gcount();
        if (read > 0) {
            crypto_blake2b_update(&hash, reinterpret_cast<const uint8_t*>(block.data()),
                                  static_cast<size_t>(read));
        }
    }
    if (in.bad()) return fail(Errc::Unavailable);
    FileId id{};
    crypto_blake2b_final(&hash, id.data());
    return id;
}

FileId file_id(std::span<const uint8_t> bytes) noexcept {
    FileId id{};
    crypto_blake2b(id.data(), id.size(), bytes.data(), bytes.size());
    return id;
}

std::string local_file_name(std::string name) {
    for (char& c : name) {
        if (std::string_view("<>:\"|?*/\\").find(c) != std::string_view::npos) c = '_';
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) return "file";
    // CON, PRN, AUX, NUL, COM1–9 and LPT1–9, whatever follows the first dot.
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool numbered = stem.size() == 4 &&
                          (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
                          stem[3] <= '9';
    if (numbered || stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        name.insert(0, 1, '_');
    return name;
}

std::string to_hex(const FileId& id) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(id.size() * 2);
    for (const uint8_t byte : id) {
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 0xF];
    }
    return hex;
}

std::optional<FileId> file_id_of_hex(std::string_view hex) noexcept {
    FileId id{};
    if (hex.size() != id.size() * 2) return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < id.size(); ++i) {
        const int high = digit(hex[2 * i]);
        const int low = digit(hex[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        id[i] = static_cast<uint8_t>(high << 4 | low);
    }
    return id;
}

}  // namespace ratcast
