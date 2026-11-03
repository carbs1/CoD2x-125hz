#include "tickrate_patch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

static void require(bool okay) {
    if (!okay) { std::fputs("patch planner assertion failed\n",stderr); std::exit(1); }
}
int main() {
    std::vector<tickrate::Patch> plan;
    for (const uint32_t target : {0x2000u,0x800u,0xfffffff0u}) {
        tickrate::redirect(plan,0x1000,"branch",{1,2,3,4,5,6,7},reinterpret_cast<void*>(target),0xe9);
        const auto& p=plan.back();
        int32_t displacement;
        std::memcpy(&displacement,p.replacement.data()+1,4);
        require(p.original==std::vector<unsigned char>({1,2,3,4,5,6,7}));
        require(p.replacement[0]==0xe9 && p.replacement[5]==0x90 && p.replacement[6]==0x90);
        require(static_cast<uint32_t>(p.address+5+displacement)==target);
    }
    tickrate::operand(plan,0x1000,"operand",{0xaa,0,0,0,0,0xbb},1,4,0x12345678);
    require(plan.back().replacement==std::vector<unsigned char>({0xaa,0x78,0x56,0x34,0x12,0xbb}));
    tickrate::operand(plan,0x1000,"byte",{0xaa,0,0xbb},1,1,0xfe);
    require(plan.back().replacement==std::vector<unsigned char>({0xaa,0xfe,0xbb}));
    for (const size_t offset : {size_t(4),size_t(6),std::numeric_limits<size_t>::max()}) {
        tickrate::operand(plan,0x1000,"invalid range",{1,2,3,4,5},offset,4,0);
        require(plan.back().replacement.empty());
    }
    for (const size_t width : {size_t(0),size_t(5)}) {
        tickrate::operand(plan,0x1000,"invalid width",{1,2,3,4,5},0,width,0);
        require(plan.back().replacement.empty());
    }
    tickrate::redirect(plan,0x1000,"short branch",{1,2,3,4},reinterpret_cast<void*>(0x2000));
    require(plan.back().replacement.empty());
    std::puts("patch planner: valid branches/operands and malformed ranges passed");
}
