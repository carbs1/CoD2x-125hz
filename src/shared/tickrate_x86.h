#ifndef COD2X_TICKRATE_X86_H
#define COD2X_TICKRATE_X86_H
#include <cstdint>
#include <vector>
namespace tickrate {
inline std::vector<unsigned char> archiveRemainder(uint32_t divisor) {
    if (!divisor || divisor > 0x7fffffff) return {};
    // Input ECX; output ECX=remainder, EDX=quotient, EAX=quotient*divisor.
    // EBX and ESP survive; final subtraction has the original remainder flags.
    std::vector<unsigned char> bytes{
        0x89,0xc8,0x99,0x53,0xbb,0,0,0,0,0xf7,0xfb,0x0f,0xaf,0xd8,0x29,0xd9,
        0x89,0xc2,0x89,0xd8,0x5b,0x90,0x90,0x90,0x90,0x90,0x90};
    for (int i = 0; i < 4; ++i) bytes[5+i] = static_cast<unsigned char>(divisor >> (8*i));
    return bytes;
}

// x86 register encoding: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI.
// clientSource=8 reads the caller's [EBP+8] instead of a register.
inline std::vector<unsigned char> frameAdapter(int sequenceReg, int clientSource,
                                             int outputReg, uint32_t target) {
    constexpr unsigned char saved[] = {28,24,20,16,12,8,4,0};
    if (sequenceReg < -1 || sequenceReg > 7 || sequenceReg == 4 ||
        clientSource < 0 || clientSource > 8 || clientSource == 4 ||
        outputReg < 0 || outputReg > 7 || outputReg == 4) return {};
    std::vector<unsigned char> bytes{0x9c,0x60,0x89,0xe3};
    if (sequenceReg < 0) bytes.insert(bytes.end(), {0x6a,0});
    else bytes.insert(bytes.end(), {0xff,0x73,saved[sequenceReg]});
    if (clientSource == 8) bytes.insert(bytes.end(), {0x8b,0x43,8,0xff,0x70,8});
    else bytes.insert(bytes.end(), {0xff,0x73,saved[clientSource]});
    bytes.push_back(0xb8);
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(target >> (8*i)));
    bytes.insert(bytes.end(), {0xff,0xd0,0x83,0xc4,8,0x89,0x43,saved[outputReg],0x89,0xdc,0x61,0x9d,0xc3});
    return bytes;
}
}
#endif
