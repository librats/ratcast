// Where there is no implementation of TS feeds yet.

#include "ratcast/engine/ts_io.h"

namespace ratcast {

Result<std::unique_ptr<TsInput>> open_ts_input(std::string_view, Clock&) {
    return fail(Errc::Unavailable);
}

Result<std::unique_ptr<TsOutput>> open_ts_output(std::string_view) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast
