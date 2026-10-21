#include "netsim/link.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace ratcast::netsim {
namespace {

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> words;
    size_t at = 0;
    while (at < text.size()) {
        const size_t start = text.find_first_not_of(" \t", at);
        if (start == std::string_view::npos) break;
        const size_t end = std::min(text.find_first_of(" \t", start), text.size());
        words.push_back(text.substr(start, end - start));
        at = end;
    }
    return words;
}

/// A number and what follows it: "25" and "ms" of "25ms". Not from_chars,
/// which not every standard library reads floating point with yet.
std::optional<std::pair<double, std::string_view>> parse_number(std::string_view word) {
    const std::string text(word);
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value) || value < 0) return std::nullopt;
    return std::pair{value, word.substr(static_cast<size_t>(end - text.c_str()))};
}

std::optional<Duration> parse_time(std::string_view word) {
    const auto parsed = parse_number(word);
    if (!parsed) return std::nullopt;
    const auto [value, unit] = *parsed;
    double us = 0;
    if (unit == "us") {
        us = value;
    } else if (unit == "ms") {
        us = value * 1e3;
    } else if (unit == "s") {
        us = value * 1e6;
    } else {
        return std::nullopt;
    }
    return Duration(static_cast<Duration::rep>(std::llround(us)));
}

/// "2%" or "2", as a share.
std::optional<double> parse_percent(std::string_view word) {
    const auto parsed = parse_number(word);
    if (!parsed || parsed->first > 100 || (!parsed->second.empty() && parsed->second != "%")) {
        return std::nullopt;
    }
    return parsed->first / 100;
}

std::optional<uint64_t> parse_rate(std::string_view word) {
    const auto parsed = parse_number(word);
    if (!parsed) return std::nullopt;
    const auto [value, unit] = *parsed;
    double scale = 0;
    if (unit == "bit") {
        scale = 1;
    } else if (unit == "kbit") {
        scale = 1e3;
    } else if (unit == "mbit") {
        scale = 1e6;
    } else if (unit == "gbit") {
        scale = 1e9;
    } else {
        return std::nullopt;  // tc's "bps" is bytes: not taken, to be read one way only
    }
    const double bps = value * scale;
    if (bps < 1) return std::nullopt;
    return static_cast<uint64_t>(std::llround(bps));
}

}  // namespace

std::optional<LinkConfig> parse_link(std::string_view options, std::string* error) {
    const std::vector<std::string_view> words = split(options);
    LinkConfig config;
    if (words.empty()) return config;
    config.limit = 1000;  // netem's queue

    size_t i = 0;
    const auto next = [&]() -> std::optional<std::string_view> {
        if (i == words.size()) return std::nullopt;
        return words[i++];
    };
    // The next word if it reads as `parse` does, which takes it.
    const auto maybe = [&](auto parse) -> decltype(parse(std::string_view{})) {
        if (i == words.size()) return std::nullopt;
        auto value = parse(words[i]);
        if (value) ++i;
        return value;
    };
    const auto fail = [&](std::string why) -> std::optional<LinkConfig> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };

    while (const std::optional<std::string_view> word = next()) {
        if (*word == "delay") {
            const std::optional<Duration> delay = maybe(parse_time);
            if (!delay) return fail("delay wants a time, as 25ms");
            config.delay = *delay;
            if (const std::optional<Duration> jitter = maybe(parse_time)) config.jitter = *jitter;
            if (i < words.size() && words[i] == "distribution") {
                ++i;
                const std::optional<std::string_view> name = next();
                if (name == "normal") {
                    config.distribution = LinkConfig::Jitter::Normal;
                } else if (name == "uniform") {
                    config.distribution = LinkConfig::Jitter::Uniform;
                } else {
                    return fail("distribution: normal or uniform");
                }
            }
        } else if (*word == "loss") {
            if (i < words.size() && words[i] == "gemodel") {
                ++i;
                LinkConfig::Bursts bursts;
                const std::optional<double> enter = maybe(parse_percent);
                if (!enter) return fail("loss gemodel wants P [R [1-H [1-K]]], as 1% 25%");
                bursts.enter = *enter;
                bursts.leave = 1 - *enter;  // netem's default
                if (const auto leave = maybe(parse_percent)) {
                    bursts.leave = *leave;
                    if (const auto bad = maybe(parse_percent)) {
                        bursts.loss_bad = *bad;
                        if (const auto good = maybe(parse_percent)) bursts.loss_good = *good;
                    }
                }
                config.bursts = bursts;
                continue;
            }
            if (i < words.size() && words[i] == "random") ++i;
            const std::optional<double> loss = maybe(parse_percent);
            if (!loss) return fail("loss wants a percentage, as 2%");
            config.loss = *loss;
        } else if (*word == "reorder") {
            const std::optional<double> reorder = maybe(parse_percent);
            if (!reorder) return fail("reorder wants a percentage, as 2%");
            config.reorder = *reorder;
        } else if (*word == "rate") {
            const std::optional<uint64_t> bps = maybe(parse_rate);
            if (!bps) return fail("rate wants bit, kbit, mbit or gbit, as 20mbit");
            config.rate_bps = *bps;
        } else if (*word == "limit") {
            uint32_t limit = 0;
            const std::optional<std::string_view> value = next();
            const auto [end, ec] =
                value ? std::from_chars(value->data(), value->data() + value->size(), limit)
                      : std::from_chars_result{nullptr, std::errc::invalid_argument};
            if (!value || ec != std::errc{} || end != value->data() + value->size() || limit == 0) {
                return fail("limit wants a number of packets");
            }
            config.limit = limit;
        } else {
            return fail("netsim does not know '" + std::string(*word) + "'");
        }
    }
    if (config.reorder > 0 && config.delay == Duration::zero()) {
        return fail("reorder needs a delay to leave before");  // as netem says
    }
    return config;
}

Link::Link(LinkConfig config, uint64_t seed) : config_(std::move(config)), random_(seed) {}

std::optional<Timestamp> Link::enter(size_t bytes, Timestamp now) {
    ++stats_.packets;
    stats_.bytes += bytes;
    if (lose()) {
        ++stats_.lost;
        return std::nullopt;
    }
    while (!held_.empty() && held_.top() <= now) held_.pop();
    if (config_.limit != 0 && held_.size() >= config_.limit) {
        ++stats_.overflowed;
        return std::nullopt;
    }

    Timestamp leaves = now;
    if (!chance(config_.reorder)) {
        leaves = now + delay();
        if (config_.rate_bps != 0) {
            // In order: not before the last held has left, then its own size
            // at the rate.
            if (!held_.empty()) leaves = std::max(leaves, latest_);
            const double seconds = double((bytes + kHeaderBytes) * 8) / double(config_.rate_bps);
            leaves += Duration(static_cast<Duration::rep>(std::llround(seconds * 1e6)));
        }
    }
    latest_ = held_.empty() ? leaves : std::max(latest_, leaves);
    held_.push(leaves);
    return leaves;
}

// netem's order: in the good state a packet may send the link to the bad one,
// and is lost by the good state's share; in the bad, it may bring it back, and
// is lost by the bad state's share.
bool Link::lose() {
    if (chance(config_.loss)) return true;
    if (!config_.bursts) return false;
    const LinkConfig::Bursts& b = *config_.bursts;
    if (!bad_) {
        if (chance(b.enter)) bad_ = true;
        return chance(b.loss_good);
    }
    if (chance(b.leave)) bad_ = false;
    return chance(b.loss_bad);
}

Duration Link::delay() {
    if (config_.jitter == Duration::zero()) return config_.delay;
    const double jitter = double(config_.jitter.count());
    double offset = 0;
    if (config_.distribution == LinkConfig::Jitter::Normal) {
        // netem's table spans four sigmas each way.
        offset = std::clamp(normal_(random_), -4.0, 4.0) * jitter;
    } else {
        offset = (unit_(random_) * 2 - 1) * jitter;
    }
    const double us = std::max(0.0, double(config_.delay.count()) + offset);
    return Duration(static_cast<Duration::rep>(std::llround(us)));
}

}  // namespace ratcast::netsim
