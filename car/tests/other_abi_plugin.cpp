// A driving-logic plug-in built for the previous ABI, for test_pack: a complete
// API table (logic_api.h) whose abi_version is RUMI_LOGIC_ABI_VERSION - 1, as a
// plug-in compiled against an older logic_api.h reports it.  The core must
// refuse to load it even when its manifest claims the current ABI: the loader
// requires the plug-in's own ABI to equal the core's (core/src/logic_loader.cpp),
// because the same fields carry other values from one ABI to the next (from
// ABI 5 the ToF readings are no longer cut off at 200 mm).
#include "rumicar/logic_api.h"

namespace {

int g_instance = 0;

void* other_create(const char*, char*, size_t) { return &g_instance; }
void other_destroy(void*) {}
int other_self_test(void*, char*, size_t) { return 0; }
int other_step(void*, const rumi_sensor_frame*, rumi_command* out) {
    *out = rumi_command{};
    out->size = sizeof(rumi_command);
    out->steer = RUMI_STEER_CENTER;
    out->drive_dir = RUMI_DRIVE_BRAKE;
    return 0;
}
const char* other_state_name(void*, uint8_t) { return "OTHER_ABI"; }
const char* other_aux_name(void*, uint8_t) { return nullptr; }

const rumi_logic_api kApi = {
    RUMI_LOGIC_ABI_VERSION - 1u,
    "other_abi",
    "1.0.0",
    &other_create,
    &other_destroy,
    &other_self_test,
    &other_step,
    &other_state_name,
    &other_aux_name,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const rumi_logic_api* rumi_logic_get_api(void) {
    return &kApi;
}
