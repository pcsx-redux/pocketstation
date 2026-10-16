// Drives mentalfoundry/Pokket's pk_timing_bench (MIT) on this core, the way their
// tools/bench_probe.c drives theirs: synthesize a one-app card around the .mcs, boot the retail
// kernel, press Down/Action/Right/Action to launch the app, page with Right, and read the result
// block the app leaves in WRAM (pk_timing_bench/src/constants.inc). Prints our value next to the
// real-unit value of their VERIFICATION.md.
//   bench <kernel.bin> <pk_timing_bench.mcs> [stop_seconds]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pocketstation.h"

namespace {

constexpr uint64_t kRefHz = 1056000;      // Pokket's reference clock; one LCD frame = kRefHz/32
constexpr uint64_t kFrame = kRefHz / 32;
// INT_INPUT bit order, which the app tests directly (ui.s poll_buttons).
constexpr uint32_t FIRE = 1, RIGHT = 2, LEFT = 4, DOWN = 8, UP = 16;

PCSX::PocketStation::PocketStation dev;
uint64_t accum = 0;

void runFrames(uint64_t n, uint32_t buttons) {
    for (uint64_t i = 0; i < n; i++) {
        dev.setButtons(buttons);
        accum += kFrame * dev.armClockHz();
        const uint64_t arm = accum / kRefHz;
        accum -= arm * kRefHz;
        dev.runCycles(arm);
    }
}

uint32_t w(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, dev.wramData() + a, 4);
    return v;
}

std::vector<uint8_t> readFile(const char* p) {
    std::vector<uint8_t> d;
    FILE* f = std::fopen(p, "rb");
    if (!f) return d;
    std::fseek(f, 0, SEEK_END);
    d.resize(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    std::fclose(f);
    return d;
}

uint8_t frameXor(const uint8_t* f) {
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; i++) x ^= f[i];
    return x;
}

// Same card that Pokket's flash_load_app synthesizes: header, a chain from slot 1, data from block 1.
std::vector<uint8_t> cardFromMcs(const std::vector<uint8_t>& mcs) {
    std::vector<uint8_t> card(128 * 1024, 0);
    const size_t size = mcs.size() - 0x80;
    card[0] = 'M';
    card[1] = 'C';
    card[0x7F] = frameXor(&card[0]);
    const uint32_t blocks = (size + 0x1FFF) / 0x2000;
    for (uint32_t b = 1; b < 16; b++) {
        uint8_t* fr = &card[b * 0x80];
        if (b <= blocks) {
            fr[0] = b == 1 ? 0x51 : b == blocks ? 0x53 : 0x52;
            if (b == 1) {
                std::memcpy(fr, mcs.data(), 0x80);  // keep their frame (name etc.), fix the link below
                fr[0] = 0x51;
                for (int i = 0; i < 4; i++) fr[4 + i] = uint8_t(size >> (8 * i));
                fr[0x10] = 'P';
            }
            const uint32_t link = b < blocks ? b : 0xFFFF;
            fr[8] = uint8_t(link);
            fr[9] = uint8_t(link >> 8);
        } else {
            fr[0] = 0xA0;
            fr[8] = fr[9] = 0xFF;
        }
        fr[0x7F] = frameXor(fr);
    }
    std::memcpy(&card[0x2000], mcs.data() + 0x80, size);
    return card;
}

void printLcd() {
    const uint8_t* v = dev.vram();
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) std::putchar((v[y * 4 + x / 8] >> (x % 8)) & 1 ? '#' : '.');
        std::putchar('\n');
    }
}

int screen() { return int(w(0x25C)); }

// Tap Right until the app's screen index is `target` (bench_probe's method).
bool pageTo(int target) {
    for (int i = 0; i < 24 && screen() != target; i++) {
        runFrames(10, RIGHT);
        runFrames(20, 0);
    }
    return screen() == target;
}

// The core exits through Helpers::panic on an unmodeled register; dump the raw result block then.
void dumpWramAtExit() {
    std::printf("\n== WRAM result words at exit (0x200-0x2D7, 0x540-0x56F) ==\n");
    for (uint32_t a = 0x200; a < 0x2D8; a += 16)
        std::printf("%03X: %08X %08X %08X %08X\n", a, w(a), w(a + 4), w(a + 8), w(a + 12));
    for (uint32_t a = 0x540; a < 0x570; a += 16)
        std::printf("%03X: %08X %08X %08X %08X\n", a, w(a), w(a + 4), w(a + 8), w(a + 12));
    std::fflush(stdout);
}

int fails = 0;
void row(const char* scr, const char* what, uint32_t ours, uint32_t hw, uint32_t tol = 1) {
    const uint32_t d = ours > hw ? ours - hw : hw - ours;
    const bool ok = d <= tol;
    if (!ok) fails++;
    std::printf("%-4s %-34s ours=0x%08X hw=0x%08X diff=%+d %s\n", scr, what, ours, hw, int(ours - hw),
                ok ? "match" : "MISMATCH");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <kernel.bin> <app.mcs> [stop_seconds]\n", argv[0]);
        return 77;
    }
    auto kernel = readFile(argv[1]);
    auto mcs = readFile(argv[2]);
    if (kernel.size() != 16384 || mcs.size() <= 0x80 || (mcs.size() - 0x80) % 0x2000 ||
        mcs.size() - 0x80 > 15 * 0x2000 || std::memcmp(&mcs[0x80 + 0x52], "MCX", 3)) {
        std::fprintf(stderr, "bad inputs (kernel %zu bytes, mcs %zu bytes)\n", kernel.size(), mcs.size());
        return 77;
    }
    const int stopSeconds = argc > 3 ? std::atoi(argv[3]) : 10;
    if (stopSeconds < 1) {
        std::fprintf(stderr, "stop_seconds must be a positive integer\n");
        return 77;
    }
    std::atexit(dumpWramAtExit);
    auto card = cardFromMcs(mcs);
    dev.setKernel(kernel.data(), kernel.size());
    dev.setFlash(card.data(), card.size());
    dev.reset();

    // Launch script: tokens "<frames><key>", key one of - F R L D U (e.g. "20- 16D 14- 16F").
    // Default: the documented cold-boot sequence below.
    std::vector<uint32_t> script;
    if (const char* s = std::getenv("BENCH_LAUNCH")) {
        while (*s) {
            char* end;
            const long n = std::strtol(s, &end, 10);
            if (end == s) { s++; continue; }
            uint32_t b = 0;
            switch (*end) {
                case 'F': b = FIRE; break;
                case 'R': b = RIGHT; break;
                case 'L': b = LEFT; break;
                case 'D': b = DOWN; break;
                case 'U': b = UP; break;
                default: break;
            }
            script.insert(script.end(), size_t(n), b);
            s = *end ? end + 1 : end;
        }
    } else {
        // Pokket docs/app-notes.md: wait out HELLO/heart (~335 frames here), Down, Action, Right,
        // Action, each a short (~40ms = 2 frame) press. bench_probe's 16-frame repeating presses
        // do not get through the kernel UI on this core.
        const std::pair<int, uint32_t> seq[] = {{360, 0}, {2, DOWN}, {30, 0}, {2, FIRE}, {60, 0},
                                                {2, RIGHT}, {60, 0}, {2, FIRE}, {300, 0}};
        for (auto [n, b] : seq) script.insert(script.end(), size_t(n), b);
    }
    for (int f = 0; f < int(script.size()); f++) {
        const uint32_t b = script[f];
        runFrames(1, b);
        if (std::getenv("BENCH_TRACE")) {
            static std::vector<std::vector<uint8_t>> seen;
            static int lastId = -1;
            std::vector<uint8_t> cur(dev.vram(), dev.vram() + 128);
            int id = -1;
            for (size_t k = 0; k < seen.size(); k++)
                if (seen[k] == cur) id = int(k);
            const bool fresh = id < 0;
            if (fresh) {
                id = int(seen.size());
                seen.push_back(cur);
            }
            if (std::getenv("BENCH_TRACE_REGS")) {
                static uint64_t lastInsn = 0;
                std::printf("  f%d b%u lcdmode=%08X mask=%08X input=%08X insn+%llu\n", f, b, dev.lcdMode(),
                            dev.peekIrqMask(), dev.peekIntInput(),
                            (unsigned long long)(dev.instructionsExecuted() - lastInsn));
                lastInsn = dev.instructionsExecuted();
            }
            if (id != lastId) {
                std::printf("frame %d buttons %u armHz %u halted %d -> lcd #%d\n", f, b, dev.armClockHz(),
                            dev.halted(), id);
                if (fresh) printLcd();
                lastId = id;
            }
        }
    }
    std::printf("after launch: screen index=%d halted=%d armHz=%u\n", screen(), dev.halted(), dev.armClockHz());
    printLcd();

    std::printf("\n== startup result block ==\n");
    row("1", "ARM loop (test)", w(0x200), 0x2BF2);
    row("1", "Thumb loop (control)", w(0x204), 0x19A3);
    row("2", "FLASH_CTRL (test)", w(0x208), 0x2BF2);
    row("2", "WRAM (control)", w(0x20C), 0x2BF2);
    row("3", "BIOS ARM helper (test)", w(0x210), 0x802D);
    row("3", "WRAM copy (control)", w(0x214), 0x6A33);
    row("4", "BIOS Thumb helper (test)", w(0x218), 0xB71B);
    row("4", "WRAM copy (control)", w(0x21C), 0xB371);
    const uint32_t d0 = w(0x280), d1 = w(0x284), d2 = w(0x288), d3 = w(0x28C);
    std::printf("5    raw snapshots ours=%04X %04X %04X %04X hw=FFEF FFEE 6269 E23C (raw, not compared)\n", d0, d1,
                d2, d3);
    row("5", "(full_before-full_after)&0xFFFF", (d2 - d3) & 0xFFFF, 0x802D);
    row("6", "Timer2 period 1016x256 (test)", w(0x290), 0x3F90);
    row("6", "Timer2 period 2032x128 (control)", w(0x294), 0x3F87);
    row("7", "IRQs masked (test)", w(0x298), 0x2BF2);
    row("7", "one timer IRQ live (control)", w(0x29C), 0x2D56);
    row("8", "re-arm over IRQ, 64 periods", w(0x2A0), 0x0FE4);
    std::printf("8    handler count %u (expect 64); control row is the constant 0x3F8 the app writes\n", w(0x2A4));
    row("9", "IRDA_DATA write (test)", w(0x2A8), 0x2BF2);
    row("9", "WRAM write (control)", w(0x2AC), 0x2849);
    row("10", "re-arm over FIQ, 64 periods", w(0x2B0), 0x0FE4);
    std::printf("10   handler count %u (expect 64); control row is the constant 0x3F8\n", w(0x2B4));
    row("11", "full-dispatch FIQ, 64 periods", w(0x2B8), 0x0FE4);
    std::printf("11   handler count %u (expect 64); control row is the constant 0x3F8\n", w(0x2BC));
    row("14", "RTC paused, 256 toggles", w(0x2D0), 0x0F3F);
    row("14", "RTC running", w(0x2D4), 0x3D00);
    {
        const uint32_t ctl = w(0x29C), tst = w(0x298);
        std::printf("7    derived interrupt cost: ours %.1f raw cycles, hw ~98\n", 3200.0 * (1.0 - double(tst) / ctl));
        for (uint32_t a : {0x2A0u, 0x2B0u, 0x2B8u})
            std::printf("     0x%03X -> effective period %.1f ticks, latency %.1f (hw 1017 / 0)\n", a,
                        w(a) * 32.0 / 64.0 / 2.0, w(a) * 32.0 / 64.0 / 2.0 - 1017.0);
    }

    // Screens 16-18: page there so draw_load_screen has run, then read the 12 words at 0x540.
    std::printf("\n== screens 16-18 ==\n");
    static const uint32_t hwLoads[12] = {0x11843322, 0x33221184, 0x11843322, 0x33221184, 0x11000022, 0x33000084,
                                         0x11000022, 0x33000084, 0x00000022, 0xFFFFFF84, 0x00000022, 0xFFFFFF84};
    static const char* names[4] = {"ARM +1", "ARM +3", "Thumb +1", "Thumb +3"};
    static const char* ins[3] = {"LDR", "LDRH", "LDRSH"};
    for (int s = 16; s <= 18; s++) {
        const bool ok = pageTo(s);
        std::printf("paged to %d: %s\n", s, ok ? "yes" : "no");
        if (s == 18) printLcd();
    }
    for (int i = 0; i < 12; i++) {
        char what[40];
        std::snprintf(what, sizeof what, "%s %s", ins[i / 4], names[i % 4]);
        char scr[4];
        std::snprintf(scr, sizeof scr, "%d", 16 + i / 4);
        row(scr, what, w(0x540 + 4 * i), hwLoads[i], 0);
    }

    // Screen 13: Down runs the single CLK-stop store; wait, then Action wakes it.
    std::printf("\n== screen 13 (stop %d s, then Action) ==\n", stopSeconds);
    if (pageTo(13)) {
        runFrames(10, DOWN);
        runFrames(20, 0);
        std::printf("halted after Down: %d\n", dev.halted());
        runFrames(uint64_t(stopSeconds) * 32 - 30, 0);
        runFrames(10, FIRE);
        runFrames(60, 0);
        std::printf("halted after Action: %d\n", dev.halted());
        row("13", "seconds stopped", w(0x2C4), uint32_t(stopSeconds));
        row("13", "Timer1 IRQs while stopped", w(0x2C8), 0, 0);
        row("13", "CLK control readback", w(0x2CC), 0x17, 0);
        printLcd();
    } else {
        std::printf("could not page to 13 (index %d)\n", screen());
    }
    std::printf("\nscreen 15 needs a PS1 accessing the slot; not run.\n");
    std::printf("mismatches (beyond +-1 jitter): %d\n", fails);
    return fails ? 1 : 0;
}
