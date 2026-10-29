// A desktop needs no telling of a call: see in_call.h.

#include "ratcast/engine/in_call.h"

namespace ratcast {
namespace {

class NothingToTell final : public InCall {
public:
    void update() override {}
};

}  // namespace

std::unique_ptr<InCall> enter_call() { return std::make_unique<NothingToTell>(); }

}  // namespace ratcast
