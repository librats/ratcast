// What WebRTC's AudioProcessing provides, in a build without it: nothing to open.

#include "ratcast/engine/audio_processing.h"

namespace ratcast {

Result<std::unique_ptr<AudioProcessor>> open_audio_processor(const AudioProcessingConfig&, Clock&) {
    return fail(Errc::Unavailable);
}

}  // namespace ratcast
