// Misaligned load results of the ARM7TDMI, Thumb and ARM state.
// Ported from mentalfoundry/Pokket tests/cpu_test.c (MIT, Copyright (c) 2026 Darien Liu), commit
// 6f38559 ("core: rotate misaligned Thumb loads and LDRH in 32 bits"): test_thumb_ldr_misaligned_rotates,
// test_thumb_ldrh_misaligned_quirks and the misaligned LDRH/LDRSH steps of test_arm_ldrh_misaligned_quirks.
// Their pk_timing_bench screens 16-18 record the same values on a real unit.
//   make -C tests/cpu check
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "cpu.h"
#include "lcd.h"

// The CPU keeps its state private and has no test hooks. An explicit instantiation may name a
// private member, so this reaches the few it needs without editing the core.
template <typename Tag, typename Tag::type M>
struct Rob {
    friend typename Tag::type get(Tag) { return M; }
};
#define ROB(Tag, T, member)                \
    struct Tag {                           \
        using type = T CPU::*;             \
        friend type get(Tag);              \
    };                                     \
    template struct Rob<Tag, &CPU::member>;
using Regs = std::array<u32, 16>;
using Callback = void (CPU::*)(u32);
using ArmLut = std::array<Callback, 4096>;
using ThumbLut = std::array<Callback, 256>;
ROB(RegsTag, Regs, registers)
ROB(ReadTag, uintptr_t*, readTable)
ROB(WriteTag, uintptr_t*, writeTable)
ROB(ArmTag, ArmLut, ARM_LUT)
ROB(ThumbTag, ThumbLut, Thumb_LUT)
#undef ROB

namespace {

struct Rig {
    LCD lcd;
    Bus bus{lcd};
    CPU cpu{bus};
    Rig() {
        cpu.reset();
        // Page 0 (0x000-0x7FF) as plain RAM for the test data.
        (cpu.*get(ReadTag{}))[0] = (uintptr_t)bus.wram.data();
        (cpu.*get(WriteTag{}))[0] = (uintptr_t)bus.wram.data();
        bus.wram.fill(0);
    }
    void put32(u32 a, u32 v) { std::memcpy(&bus.wram[a], &v, 4); }
    void thumb(u16 i) { (cpu.*(cpu.*get(ThumbTag{}))[i >> 8])(i); }
    void arm(u32 i) { (cpu.*(cpu.*get(ArmTag{}))[((i >> 4) & 0xF) | ((i >> 16) & 0xFF0)])(i); }
    u32& r(int n) { return (cpu.*get(RegsTag{}))[n]; }
};

int failures = 0;

bool expect(const char* what, u32 got, u32 want) {
    std::printf("  %-34s got %08X want %08X  %s\n", what, got, want, got == want ? "ok" : "FAIL");
    return got == want;
}

// Each case runs in a child, so a Helpers::panic in one does not hide the others.
void run(const char* name, bool (*fn)()) {
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        const bool ok = fn();
        std::fflush(stdout);
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    const bool pass = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) failures++;
}

const u16 kLdrReg = 0x5800 | (2 << 6) | (1 << 3) | 0;   // LDR  r0,[r1,r2]
const u16 kLdrhReg = 0x5A00 | (2 << 6) | (1 << 3) | 0;  // LDRH r0,[r1,r2]
const u16 kLdshReg = 0x5E00 | (2 << 6) | (1 << 3) | 5;  // LDSH r5,[r1,r2]

void wordData(Rig& t) { t.put32(0x100, 0x0F0E0D0C); t.put32(0x104, 0x25251110); }
void halfData(Rig& t) { t.put32(0x100, 0x80332211); }

bool thumbLdrReg() {
    Rig t; wordData(t); t.r(1) = 0x100; bool ok = true;
    const u32 want[] = {0x25251110, 0x0C0F0E0D, 0x0D0C0F0E, 0x0E0D0C0F};
    const u32 off[] = {4, 1, 2, 3};
    for (int k = 0; k < 4; k++) {
        t.r(2) = off[k]; t.thumb(kLdrReg);
        char s[64]; std::snprintf(s, sizeof s, "LDR r0,[r1,r2] @%03X", 0x100 + off[k]);
        ok &= expect(s, t.r(0), want[k]);
    }
    return ok;
}
bool thumbLdrImm() {
    Rig t; wordData(t); t.r(4) = 0x101;
    t.thumb(0x6800 | (1 << 6) | (4 << 3) | 3);  // LDR r3,[r4,#4]
    return expect("LDR r3,[r4,#4] @105", t.r(3), 0x10252511);
}
bool thumbLdrSp() {
    Rig t; wordData(t); t.r(13) = 0x102;
    t.thumb(0x9800 | (5 << 8) | 1);  // LDR r5,[SP,#4]
    return expect("LDR r5,[SP,#4] @106", t.r(5), 0x11102525);
}
bool thumbLdrhReg() {
    Rig t; halfData(t); t.r(1) = 0x100; bool ok = true;
    t.r(2) = 0; t.thumb(kLdrhReg); ok &= expect("LDRH r0,[r1,r2] @100", t.r(0), 0x00002211);
    t.r(2) = 1; t.thumb(kLdrhReg); ok &= expect("LDRH r0,[r1,r2] @101", t.r(0), 0x11000022);
    t.r(2) = 3; t.thumb(kLdrhReg); ok &= expect("LDRH r0,[r1,r2] @103", t.r(0), 0x33000080);
    return ok;
}
bool thumbLdrhImm() {
    Rig t; halfData(t); t.r(4) = 0x101;
    t.thumb(0x8800 | (1 << 6) | (4 << 3) | 3);  // LDRH r3,[r4,#2]
    return expect("LDRH r3,[r4,#2] @103", t.r(3), 0x33000080);
}
bool thumbLdsh() {
    Rig t; halfData(t); t.r(1) = 0x100; bool ok = true;
    t.r(2) = 2; t.thumb(kLdshReg); ok &= expect("LDSH r5,[r1,r2] @102", t.r(5), 0xFFFF8033);
    t.r(2) = 3; t.thumb(kLdshReg); ok &= expect("LDSH r5,[r1,r2] @103", t.r(5), 0xFFFFFF80);
    t.r(2) = 1; t.thumb(kLdshReg); ok &= expect("LDSH r5,[r1,r2] @101", t.r(5), 0x00000022);
    return ok;
}
bool armLdrh() {
    Rig t; halfData(t); t.r(0) = 0x100; bool ok = true;
    // LDRH r2,[r0,#1]  (pre-indexed, up, immediate offset, no writeback)
    t.arm(0xE1D020B1); ok &= expect("ARM LDRH r2,[r0,#1] @101", t.r(2), 0x11000022);
    // LDRSH r3,[r0,#3]
    t.arm(0xE1D030F3); ok &= expect("ARM LDRSH r3,[r0,#3] @103", t.r(3), 0xFFFFFF80);
    return ok;
}

}  // namespace

int main() {
    run("thumb LDR  reg-offset misaligned", thumbLdrReg);
    run("thumb LDR  imm-offset misaligned", thumbLdrImm);
    run("thumb LDR  SP-relative misaligned", thumbLdrSp);
    run("thumb LDRH reg-offset misaligned", thumbLdrhReg);
    run("thumb LDRH imm-offset misaligned", thumbLdrhImm);
    run("thumb LDSH reg-offset misaligned", thumbLdsh);
    run("arm   LDRH/LDRSH misaligned", armLdrh);
    std::printf("%d failing case(s)\n", failures);
    return failures ? 1 : 0;
}
