#include "ratcast/client/cpu_meter.h"

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace ratcast {

Duration process_cpu_time() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return {};
    const auto ticks = [](FILETIME t) {  // 100 ns each
        return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return Duration{static_cast<Duration::rep>((ticks(kernel) + ticks(user)) / 10)};
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return {};
    const auto micros = [](timeval t) { return Duration{t.tv_sec * 1'000'000 + t.tv_usec}; };
    return micros(usage.ru_utime) + micros(usage.ru_stime);
#endif
}

CpuMeter::CpuMeter()
    : start_(Wall::now()), last_(start_), start_cpu_(process_cpu_time()), last_cpu_(start_cpu_) {}

double CpuMeter::take_interval() {
    const Wall::time_point now = Wall::now();
    const Duration cpu = process_cpu_time();
    const double result =
        percent(cpu - last_cpu_, std::chrono::duration_cast<Duration>(now - last_));
    last_ = now;
    last_cpu_ = cpu;
    return result;
}

double CpuMeter::total() const {
    return percent(process_cpu_time() - start_cpu_,
                   std::chrono::duration_cast<Duration>(Wall::now() - start_));
}

double CpuMeter::percent(Duration cpu, Duration wall) {
    return wall.count() > 0 ? 100.0 * double(cpu.count()) / double(wall.count()) : 0;
}

}  // namespace ratcast
