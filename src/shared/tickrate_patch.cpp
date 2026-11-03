#include "tickrate_patch.h"
#include "shared.h"
#include "cod2_common.h"
#include <cstring>
#include <utility>
#if COD2X_WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
namespace tickrate {
namespace {
bool relocated = false;
std::vector<void*> codePages;
}

void redirect(std::vector<Patch>& patches, unsigned int address, const char* name,
              std::vector<unsigned char> original, const void* target, unsigned char opcode) {
    std::vector<unsigned char> replacement;
    // A site shorter than the 5-byte branch is left empty so verification rejects it.
    if (original.size() >= 5) {
        replacement.assign(original.size(), 0x90);
        replacement[0] = opcode;
        const int32_t relative = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target)) - (address + 5);
        memcpy(replacement.data() + 1, &relative, 4);
    }
    patches.push_back({address, name, std::move(original), std::move(replacement)});
}

void operand(std::vector<Patch>& patches, unsigned int address, const char* name,
             std::vector<unsigned char> original, size_t offset, size_t width, uint32_t value) {
    std::vector<unsigned char> replacement = original;
    if (!width || width > 4 || offset > replacement.size() || width > replacement.size() - offset)
        replacement.clear(); // rejected by verification, including offset arithmetic overflow
    for (size_t i = 0; i < width && !replacement.empty(); ++i)
        replacement[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    patches.push_back({address, name, std::move(original), std::move(replacement)});
}

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
    relocated = true;
}

bool canReload() { return !relocated; }

void* executableCode(const std::vector<unsigned char>& bytes) {
    if (bytes.empty() || bytes.size() > 4096) return nullptr;
#if COD2X_WIN32
    void* page = VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!page) return nullptr;
    memcpy(page,bytes.data(),bytes.size());
    DWORD old;
    if (!VirtualProtect(page,4096,PAGE_EXECUTE_READ,&old)) {
        VirtualFree(page,0,MEM_RELEASE); return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(),page,bytes.size());
#else
    void* page = mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (page == MAP_FAILED) return nullptr;
    memcpy(page,bytes.data(),bytes.size());
    if (mprotect(page,4096,PROT_READ|PROT_EXEC)) {
        munmap(page,4096); return nullptr;
    }
#endif
    codePages.push_back(page);
    return page;
}

void releaseExecutableCode() {
    if (!canReload()) return; // installed native callers retain these addresses
    for (auto page : codePages) {
#if COD2X_WIN32
        VirtualFree(page,0,MEM_RELEASE);
#else
        munmap(page,4096);
#endif
    }
    codePages.clear();
}
}
