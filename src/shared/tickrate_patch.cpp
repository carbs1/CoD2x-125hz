#include "tickrate_patch.h"
#include "shared.h"
#include "cod2_common.h"
#include <cstring>
namespace tickrate {
bool verifyPatches(const std::vector<Patch>& patches) {
    bool valid = true;
    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& patch = patches[i];
        if (patch.original.empty() || patch.original.size() != patch.replacement.size() ||
            memcmp(reinterpret_cast<void*>(patch.address),patch.original.data(),patch.original.size())) {
            Com_Printf("TICKRATE_VERIFY_FAILED site=%s address=0x%08x; patch set unchanged\n",patch.name,patch.address);
            valid = false;
        }
        for (size_t j = 0; j < i; ++j) {
            const auto& previous = patches[j];
            if (patch.address < previous.address + previous.original.size() &&
                previous.address < patch.address + patch.original.size()) {
                Com_Printf("TICKRATE_VERIFY_FAILED overlapping sites=%s,%s; patch set unchanged\n",patch.name,previous.name);
                valid = false;
            }
        }
    }
    return valid;
}
void applyPatches(const std::vector<Patch>& patches) {
    for (const auto& patch : patches)
        patch_copy(patch.address,const_cast<unsigned char*>(patch.replacement.data()),patch.replacement.size());
}
}
