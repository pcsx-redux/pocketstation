// Red tests for hardware behaviours measured by mentalfoundry/Pokket pk_timing_bench (VERIFICATION.md, MIT); written independently.
//   make -C tests/cpu check-hw-edge   (cases the core does not model yet run as expected failures)
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "cpu.h"
#include "io.h"
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
ROB(CpsrTag, PSR, cpsr)
ROB(SpsrTag, PSR, spsr)
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
    PSR& cpsr() { return cpu.*get(CpsrTag{}); }
    PSR& spsr() { return cpu.*get(SpsrTag{}); }
};

int failures = 0;

bool expect(const char* what, u32 got, u32 want) {
    std::printf("  %-34s got %08X want %08X  %s\n", what, got, want, got == want ? "ok" : "FAIL");
    return got == want;
}

// Each case runs in a child, so a Helpers::panic in one does not hide the others. An expected
// failure prints XFAIL and does not count; if it passes it prints XPASS and counts, so the
// marker is removed once the core models the case.
void run(const char* name, bool (*fn)(), bool expectFail = false) {
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
    const char* verdict = expectFail ? (pass ? "XPASS" : "XFAIL") : (pass ? "PASS" : "FAIL");
    std::printf("%s %s\n", verdict, name);
    if (pass == expectFail) failures++;
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
// Not modelled yet: the core panics, so the child dies. Expected failure.
bool emptyList() {
    Rig t;
    t.r(0) = 0x200;
    t.arm(0xE8900000);  // LDMIA r0,{}
    return expect("base after LDMIA r0,{}", t.r(0), 0x240);
}

// The stop takes effect once 3 cycles have elapsed, not never.
bool clkStopHalts() {
    Rig t;
    for (int i = 0; i < 8; i++) t.put32(i * 4, 0xE3A01001);  // MOV r1,#1
    t.bus.write32Slow(0x0B000004, 1);
    bool ok = expect("halted before any step", t.bus.halted, 0);
    for (int i = 0; i < 8 && !t.bus.halted; i++) t.cpu.step();
    ok &= expect("halted after the window", t.bus.halted, 1);
    ok &= expect("stopped short of the end", t.r(15) < 0x28, 1);
    return ok;
}

// Same misaligned-target rule through MOV PC and LDR PC.
bool movPcKeepsBit1() {
    Rig t;
    t.r(0) = 0x102;
    t.arm(0xE1A0F000);  // MOV pc,r0
    return expect("R15 after ARM MOV pc,0x102", t.r(15), 0x10A);
}

bool ldrPcKeepsBit1() {
    Rig t;
    t.r(0) = 0x200;
    t.put32(0x200, 0x102);
    t.arm(0xE590F000);  // LDR pc,[r0]
    return expect("R15 after ARM LDR pc,[0x102]", t.r(15), 0x10A);
}

// Undefined instruction exception: vector 4, mode UND, LR = next instruction, IRQs off.
bool undefinedArm() {
    Rig t;
    t.r(15) = 0x108;  // the instruction is at 0x100
    t.arm(0xE7F000F0);
    bool ok = expect("R15 at vector 4 + 8", t.r(15), 0x0C);
    ok &= expect("mode UND", t.cpsr().mode, 0x1B);
    ok &= expect("LR = next instruction", t.r(14), 0x104);
    ok &= expect("SPSR_und = old CPSR", t.spsr().raw, 0x5F);
    ok &= expect("IRQs disabled", t.cpsr().irqDisable, 1);
    return ok;
}

// Masking a source drops its held request.
bool maskClearsHeldButton() {
    Rig t;
    t.bus.irqMask = 1;
    t.bus.requestInterrupt(0);
    bool ok = expect("latch after press", t.bus.irqLatch & 1, 1);
    t.bus.write32Slow(IO::INT_MASK_CLR, 1);
    ok &= expect("latch after INT_MASK_CLR", t.bus.irqLatch & 1, 0);
    t.bus.irqMask = 0x80;
    t.bus.irqLatch = 0x80;  // a timer expiry: not a button, left alone
    t.bus.write32Slow(IO::INT_MASK_CLR, 0x80);
    ok &= expect("non-button latch after INT_MASK_CLR", t.bus.irqLatch & 0x80, 0x80);
    return ok;
}

// Releasing a button drops its request without an acknowledge.
bool releaseClearsRequest() {
    Rig t;
    t.bus.irqMask = 1;
    t.bus.requestInterrupt(0);
    t.bus.releaseInterrupt(0);
    bool ok = expect("level after release", t.bus.irqFlags & 1, 0);
    ok &= expect("latch after release", t.bus.irqLatch & 1, 0);
    return ok;
}

}  // namespace

int main() {
    run("CLK_STOP runs ~3 cycles after the store", clkStopDelay);
    run("ARM BX keeps bit 1 of a misaligned target", bxKeepsBit1);
    run("LDM with empty rlist does not panic", emptyList, true);
    run("CLK_STOP halts once the window closes", clkStopHalts);
    run("ARM MOV pc keeps bit 1", movPcKeepsBit1);
    run("ARM LDR pc keeps bit 1", ldrPcKeepsBit1);
    run("undefined instruction takes the UND exception", undefinedArm);
    run("INT_MASK_CLR drops a held button request", maskClearsHeldButton);
    run("button release drops its request", releaseClearsRequest);
    std::printf("%d failing case(s)\n", failures);
    return failures ? 1 : 0;
}
