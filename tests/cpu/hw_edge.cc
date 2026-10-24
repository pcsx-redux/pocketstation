// Red tests for hardware behaviours measured by mentalfoundry/Pokket pk_timing_bench (VERIFICATION.md, MIT); written independently.
//   make -C tests/cpu check-hw-edge   (expected RED until the core models them)
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

// Screen 13 modes 6/7: after the CLK_STOP store the CPU keeps running for a few cycles (the next
// instruction starts while fewer than 3 cycles have passed since the store). Hardware: two 1-cycle
// MOVs after the store both execute before the stop.
bool clkStopDelay() {
    Rig t;
    t.put32(0, 0xE3A01001);  // MOV r1,#1
    t.put32(4, 0xE3A02002);  // MOV r2,#2
    t.bus.write32Slow(0x0B000004, 1);  // the CLK_STOP store's effect
    for (int i = 0; i < 2; i++) t.cpu.step();
    bool ok = expect("r1 after 1st post-store MOV", t.r(1), 1);
    ok &= expect("r2 after 2nd post-store MOV", t.r(2), 2);
    return ok;
}

// Screen 29 probes 6-D: a misaligned write to R15 keeps bit 1 in ARM state (GBATEK says forced
// aligned; hardware does not). BX r0 with r0=0x102 then has R15 = target + 8.
bool bxKeepsBit1() {
    Rig t;
    t.r(0) = 0x102;
    t.arm(0xE12FFF10);  // BX r0
    return expect("R15 after ARM BX 0x102", t.r(15), 0x10A);
}

// Screen 26: LDM with an empty register list does not fault on hardware (loads R15, base +0x40).
// The core panics, so the child dies.
bool emptyList() {
    Rig t;
    t.r(0) = 0x200;
    t.arm(0xE8900000);  // LDMIA r0,{}
    return expect("base after LDMIA r0,{}", t.r(0), 0x240);
}

}  // namespace

int main() {
    run("CLK_STOP runs ~3 cycles after the store", clkStopDelay);
    run("ARM BX keeps bit 1 of a misaligned target", bxKeepsBit1);
    run("LDM with empty rlist does not panic", emptyList);
    std::printf("%d failing case(s)\n", failures);
    return failures ? 1 : 0;
}
