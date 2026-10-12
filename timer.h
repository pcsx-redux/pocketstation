#pragma once
#include "types.h"

// One of the three timer channels at 0x0A800000 + n*0x10: period (+0x0), count (+0x4) and control
// (+0x8). Control bits 0-1 select the prescaler (0 or 3 = /2, 1 = /32, 2 = /512), bit 2 enables.
// The counter runs P, P-1 .. 0 and reloads on the tick AFTER zero, so a period P expires every
// P+1 ticks, and enabling a channel loads the counter from the period. Both registers are 16 bits
// wide. Behaviour as measured on a J110 by mentalfoundry's Pokket (docs/hardware-notes.md,
// "Timers"; pk_timing_bench screen 6). The timers run off the system clock, so CLK_STOP stops them.
struct Timer {
    u32 mode;      // control (+0x8)
    u16 counter;   // count (+0x4)
    u16 reload;    // period (+0x0)
    u32 prescale;  // system clock cycles counted toward the next counter tick

    void reset() {
        mode = 0;
        counter = 0;
        reload = 0;
        prescale = 0;
    }

    u32 divisor() const {
        switch (mode & 3) {
            case 1: return 32;
            case 2: return 512;
            default: return 2;
        }
    }

    void writeMode(u32 val) {
        const bool wasEnabled = mode & 4;
        mode = val;
        prescale = 0;  // a control write restarts the prescaler
        if (!wasEnabled && (mode & 4)) counter = reload;
    }

    // Advance by one system clock cycle. Returns true when the channel expires (and reloads).
    bool tick() {
        if (!(mode & 4) || reload == 0) return false;
        if (++prescale < divisor()) return false;
        prescale = 0;
        if (counter == 0) {
            counter = reload;
            return true;
        }
        counter--;
        return false;
    }
};
