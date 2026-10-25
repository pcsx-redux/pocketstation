#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <vector>
#include "types.h"
#include "lcd.h"
#include "state.h"
#include "timer.h"

class Bus {
    static constexpr u32 KB = 1024;
    static constexpr u32 MB = 1024 * KB;
    static constexpr u32 sectorSize = 0x2000;

    LCD& lcd;
    std::array<Timer, 3> timers;
    u32 iopData;
    u32 iopCtrl;
    u32 dacCtrl;
    u32 clkMode;
    int clkReadTries = 0;  // CLK_MODE reads since the ready bit last dropped (see read32Slow)
    bool rtcAdjustSkip = false;  // RTC_ADJUST takes every other write (see write32Slow)

    u32 rtcMode;
    struct RTC {
        u32 seconds, minutes, hours, dayOfWeek, days, months, years;

        u32 getDate() {
            return days | (months << 8) | (years << 16);
        }

        u32 getTime() {
            return seconds | (minutes << 8) | (hours << 16) | (dayOfWeek << 24);
        }

        void reset() {
            seconds = 0;
            minutes = 0;
            hours = 0;
            dayOfWeek = 1;
            days = 1;
            months = 1;
            years = 0x99;
        }
    } rtc;

    void remapFLASH();

public:
    u32 irqMask;  // Enabled interrupts
    u32 irqFlags; // Requested interrupts (raw INT_INPUT signal levels)
    u32 irqLatch; // Latched edge-triggered requests (drives CPU dispatch; cleared by INT_ACK)

    // ---- CLK_STOP sleep (battery/standby model) ----------------------------------------------
    // The PocketStation has no power switch: the closest thing to "off" is sleep mode, entered by
    // writing bit0=1 to CLK_STOP (0x0B000004). psx-spx: "Stops the CPU until an interrupt occurs."
    // When set, the CPU core's clock is stopped -- it executes NO instructions and burns ~no host
    // CPU (this is what lets an undocked device idle cheaply on battery). It also stops Timer0-2
    // (they run off the same system clock), so CPU::step() skips tickTimers() while halted. The
    // RTC, however, runs off a separate oscillator and keeps ticking: tickRtc() is advanced every
    // cycle regardless of halt, so its 1 Hz square-wave IRQ (INT_INPUT.9) -- plus Fire (IRQ-0) and
    // Docking (IRQ-11) -- can wake the device. Wake = an unmasked interrupt latches
    // (irqLatch & irqMask != 0); the core then clears halt and resumes (pollInterrupts dispatches
    // the pending IRQ, execution continues at the instruction after the CLK_STOP store). The flag
    // lives here in the Bus because CPU::step() (the halt/wake gate) and the bus devices both
    // need to see it.
    bool halted = false;
    // The store that stops the clock does not stop the core at once: the next instruction still
    // starts while fewer than 3 cycles have elapsed since the store. stopPending/stopElapsed track
    // that window; CPU::step() sets halted when it closes.
    bool stopPending = false;
    u32 stopElapsed = 0;
    std::atomic<u32> pressedButtons = 0; // The GUI thread writes here

    // The CPU core uses software fastmem
    // Splitting the address space into 2KiB (2^11) pages, so 0x200000 pages
    // Here are our page tables
    std::vector<uintptr_t> readTable;
    std::vector<uintptr_t> writeTable;

    struct {
        u32 enabledBanks;
        // For each of the 16 physical flash banks, we can choose which virtual bank to map it to
        std::array<u32, 16> bankMappings;
        std::vector<u8> data;

        u32 f_wait1;   // Undocumented (0x0600000C).

        // FLASHDataController (0x06000010). Holds the writable control bits (ENPRG/LOCK/STDBY/WAIT/
        // LOADPAGE/LOADSGN); BUSY is synthesised on read (our writes complete instantly so the
        // device is always "ready"). dataCtrl plus progState below model the flash write sequence.
        u32 dataCtrl;

        // JEDEC-style flash-program unlock state machine (PDA HW spec, flash data write sequence).
        // 0 = idle (need 0xAA -> 0x55AA), 1 = need 0x55 -> 0x2A54, 2 = need 0xA0 -> 0x55AA,
        // 3 = armed (subsequent data halfwords land in flash). Reset to 0 whenever programming is
        // disarmed (ENPRG/LOADPAGE cleared). Real NOR flash is read-only unless this sequence runs,
        // which is why flash is NOT in the CPU writeTable: a stray store can't modify it.
        int progState;
    } flash;

    // ---- Flash programming model (PDA HW spec "Write accesses", Appendix A FLASHDataController) ---
    static constexpr u32 kFlashBase = 0x08000000;   // physical flash; absolute sector 0.
    static constexpr u32 kFlashSize = 128 * KB;
    static constexpr u32 kFlashUnlockA = 0x080055AA;  // JEDEC command address 1.
    static constexpr u32 kFlashUnlockB = 0x08002A54;  // JEDEC command address 2.
    // FLASHDataController bits.
    static constexpr u32 kFlashENPRG = 1u << 0;
    static constexpr u32 kFlashLOCK = 1u << 1;
    static constexpr u32 kFlashBUSY = 1u << 2;
    static constexpr u32 kFlashSTDBY = 1u << 3;
    static constexpr u32 kFlashWAIT = 1u << 4;
    static constexpr u32 kFlashLOADPAGE = 1u << 5;
    static constexpr u32 kFlashLOADSGN = 1u << 6;
    // Writable mask (everything the kernel can set; BUSY is read-only/synthesised).
    static constexpr u32 kFlashCtrlWritable =
        kFlashENPRG | kFlashLOCK | kFlashSTDBY | kFlashWAIT | kFlashLOADPAGE | kFlashLOADSGN;

    // One halfword write into the flash address space, routed here because flash is not directly
    // writable (it is omitted from the CPU writeTable). Faithfully models the documented program
    // sequence: programming must be armed (ENPRG && LOADPAGE && !LOCK), then the JEDEC unlock
    // (0xAA/0x55/0xA0 to the two command addresses) opens the data phase, after which up to 64
    // halfwords land in the target sector. Stores that are not part of an armed, unlocked sequence
    // are ignored (flash behaves as ROM), which is exactly the wart this fixes: a plain `str` used
    // to modify flash.data directly. The command/data interface is PHYSICAL (0x08000000-based) for
    // both absolute (SWI 16) and relative (SWI 3) writes; the kernel translates relative addresses
    // to physical before driving this.
    void flashProgramWrite(u32 addr, u16 val) {
        const bool armed = (flash.dataCtrl & kFlashENPRG) && (flash.dataCtrl & kFlashLOADPAGE) &&
                           !(flash.dataCtrl & kFlashLOCK);
        if (!armed) return;  // ROM unless programming is armed.
        switch (flash.progState) {
            case 0:
                if (addr == kFlashUnlockA && (val & 0xFF) == 0xAA) flash.progState = 1;
                return;
            case 1:
                flash.progState = (addr == kFlashUnlockB && (val & 0xFF) == 0x55) ? 2 : 0;
                return;
            case 2:
                flash.progState = (addr == kFlashUnlockA && (val & 0xFF) == 0xA0) ? 3 : 0;
                return;
            default: {  // 3: armed, the data phase. A halfword lands in the target sector.
                const u32 off = addr - kFlashBase;
                if (off + 1 < kFlashSize) *reinterpret_cast<u16*>(&flash.data[off]) = val;
                return;
            }
        }
    }

    std::array<u8, 2 * KB> wram;
    std::vector<u8> bios;

    // ---- COM link (card slot SPI bridge to the PSX) ------------------------------------------
    // The register model below follows com.c/com.h of mentalfoundry/Pokket (MIT, Copyright (c)
    // 2026 Darien Liu), whose behaviour was traced against the retail kernel and logged on real
    // hardware (screen 15 of their pk_timing_bench). The card protocol itself lives in the kernel;
    // this block only models the byte path and the handshake lines.
    //
    //   COM_MODE  bit0 data output enable, bit1 drive /ACK low (the kernel's per-byte acknowledge).
    //   COM_STAT1 bit0 a byte arrived (cleared by a COM_DATA read or by the acknowledge),
    //             bit1 the PS1 has /SEL released. Bit 1 is a LEVEL, held for as long as /SEL is
    //             released; the kernel's end-of-command wait polls it, and nothing else ends that
    //             wait. Reading the register does not clear it, the next select does.
    //   COM_DATA  read: the byte from the PS1. write: the reply for the NEXT exchange. A single
    //             shift register each way, never a queue: one exchange shifts the PS1 byte in and
    //             the previously loaded reply out, so the PS1 sees byte N during exchange N+1.
    //   COM_CTRL1 bits0-1 select which COM_STAT2 events are visible and request INT_COM (FIQ-6).
    //   COM_STAT2 sticky events: bit0 Ready (a byte completed), bit1 the PS1 selected the device.
    //             A read gives events & CTRL1. A read of COM_DATA does not clear Ready.
    //   COM_CTRL2 a write of 1 to a bit clears that COM_STAT2 event.
    // INT_COM is a level, events & CTRL1 != 0; the latch is set when it rises and cleared when it
    // falls. The kernel waits with CTRL1 = 2, so its FIQ starts at the select, before the first
    // byte completes, and then polls Ready per byte. A byte that completes at the very instant of
    // the select is cleared by the kernel's CTRL2 = 1 and gets no answer: the host must let the
    // ARM7 run between the select and the first byte.
    struct Com {
        u32 mode = 0;           // COM_MODE
        u32 ctrl1 = 0;          // COM_CTRL1
        u32 ctrl2 = 0;          // COM_CTRL2
        u8 rx = 0;              // byte from the PS1 (COM_DATA read)
        u8 tx = 0xFF;           // reply loaded by the kernel for the next exchange (COM_DATA write)
        u8 txShifted = 0xFF;    // the byte that went out during the exchange in progress
        bool rxReady = false;   // COM_STAT1.0
        bool acked = false;     // the kernel drove /ACK low for the exchange in progress
        u32 stat2Events = 0;    // COM_STAT2 sticky events (bit0 Ready, bit1 selected)
        bool irqLevel = false;  // INT_COM as last computed
        bool selected = false;  // /SEL held by the PS1
        bool selReleased = false;  // COM_STAT1.1
        bool txFresh = false;   // kernel loaded a reply since the last exchange (diagnostic only)
        u32 hostUnderruns = 0;  // exchanges that found no fresh reply loaded (diagnostic only)
        void reset() { *this = Com{}; }
    } com;

    bool comTrace = false;  // when set, cpu.cc/bus.cc log every COM/INT reg access with PC.
    u32 curPC = 0;          // last instruction fetch PC; CPU stashes it when comTrace is on.

    // Recomputes INT_COM (FIQ-6) from the COM_STAT2 events and COM_CTRL1.
    void comUpdateIrq() {
        const bool level = (com.stat2Events & com.ctrl1 & 3u) != 0;
        if (level == com.irqLevel) return;
        com.irqLevel = level;
        if (level) {
            irqFlags |= 1u << 6;
            irqLatch |= (1u << 6) & irqMask;
        } else {
            irqFlags &= ~(1u << 6);
            irqLatch &= ~(1u << 6);
        }
    }

    // The /SEL line. Holding it starts a command (COM_STAT2 bit 1); releasing it ends one
    // (COM_STAT1 bit 1, held until the next select).
    void comSetSelected(bool selected) {
        if (com.selected && !selected) {
            com.selReleased = true;
        } else if (!com.selected && selected) {
            com.selReleased = false;
            com.stat2Events |= 2u;
            comUpdateIrq();
        }
        com.selected = selected;
    }

    // One byte from the PS1. The reply for this exchange is the byte the kernel loaded before it
    // (comTakeReply). Non-blocking: the ARM7 must run afterwards for the kernel to take the byte,
    // load the next reply and acknowledge (comAcked).
    void comBeginTransfer(u8 in) {
        if (!com.txFresh) com.hostUnderruns++;
        com.txFresh = false;
        // COM_MODE bit 0 is the data output enable: with it clear the line idles high, so the PS1
        // reads 0xFF whatever COM_DATA holds (the kernel clears it at the end of every command).
        com.txShifted = (com.mode & 1) ? com.tx : 0xFF;
        com.rx = in;
        com.rxReady = true;
        com.acked = false;
        com.stat2Events |= 1u;
        comUpdateIrq();
    }
    bool comAcked() const { return com.acked; }
    u8 comTakeReply() const { return com.txShifted; }
    void comEndTransfer() {
        com.rxReady = false;
        com.acked = false;
    }

    // Compatibility wrapper: select if needed, then one exchange. Returns the reply byte.
    u8 comExchange(u8 in) {
        if (!com.selected) comSetSelected(true);
        comBeginTransfer(in);
        return com.txShifted;
    }

    // /SEL released: ends the command in progress.
    void comDeselect() { comSetSelected(false); }

    // Docking: INT_INPUT.11 reflects dock status (level), and a transition fires IRQ-11 so the
    // kernel's IOP handler runs (which, via the GUI's per-frame SWI 05h, enables COM).
    void setDocked(bool docked) {
        const bool was = (irqFlags & (1u << 11)) != 0;
        if (docked == was) return;
        if (docked) {
            // requestInterrupt sets the INT_INPUT.11 level AND latches the edge (so the IOP IRQ
            // fires once). The level then stays set while docked (INT_ACK preserves bit 11), so
            // the handler can read docked=1; the latch is acked normally -> no storm.
            requestInterrupt(11);
        } else {
            irqFlags &= ~(1u << 11);          // level goes low on undock.
            irqLatch |= (1u << 11) & irqMask;  // latch the 1->0 edge directly (don't re-set level).
        }
    }

    // ---- System clock (CLK_MODE.FREQ / PMFrequency) ------------------------------------------
    // The ARM7 system clock is software-selectable: CLK_MODE bits 0..3 (FREQ) pick one of nine
    // speeds (PDA Hardware Spec Table 2 / Appendix A PMFrequency). Map the FREQ field to actual Hz.
    // FREQ 7 (4 MHz) is the emulator's reset/boot clock and the calibration baseline -- every existing
    // test keys off it -- so freqToHz(7) MUST be exactly 3997696. (The spec's RESET value is FREQ 0 =
    // 32 KHz, but the emulator boots clkMode=7 to match the docked-as-MemoryCard state the kernel
    // assumes; see Bus::reset.) TODO: calibrate exact Hz per FREQ against silicon in phase 3; the
    // 8 MHz value is spec-ambiguous (Table 2: 7.9977 MHz, Appendix A: 7.9954 MHz) -- using 2x4MHz.
    static constexpr u32 freqToHz(u32 freq) {
        switch (freq & 0xF) {
            case 0:  return 32768;     // 32.768 KHz (spec reset default)
            case 1:  return 63488;     // 63.488 KHz
            case 2:  return 126976;    // 126.976 KHz
            case 3:  return 253952;    // 253.952 KHz
            case 4:  return 507904;    // 507.904 KHz
            case 5:  return 1015808;   // 1.015808 MHz
            case 6:  return 1998848;   // 1.998848 MHz (2 MHz nominal)
            case 7:  return 3997696;   // 3.997696 MHz (4 MHz nominal; boot clock / baseline)
            default: return 7995392;   // 8 (and higher) = 8 MHz nominal (2x the 4 MHz value)
        }
    }
    // Live ARM7 clock in Hz from the current CLK_MODE.FREQ. The RTC oscillator (rtcHalfPeriod) and
    // the host-side cycle catch-up (SIO::stepPocketstation*) both scale off this, so a software clock
    // change rescales them together and the RTC's real-time rate stays constant.
    u32 armClockHz() const { return freqToHz(clkMode); }
    // Test-only: force CLK_MODE.FREQ directly (white-box, parallels setButtons). Lets a headless
    // harness drive the RTC-real-rate invariant at a chosen clock without going through the kernel.
    void setClkModeForTest(u32 freq) { clkMode = freq & 0xF; }

    // ---- RTC square wave (INT_INPUT.9) -------------------------------------------------------
    // INT_INPUT bit 9 is the RTC square-wave LEVEL. psx-spx: ~1 Hz normally, 4096 Hz when the RTC
    // is paused via RTC_MODE.0 (0=Run/1Hz, 1=Pause/4096Hz); INT_INPUT reports the raw signal
    // levels. The retail kernel's early boot busy loop at bios 0x650..0x65e phase-aligns to this
    // edge: phase1 spins until bit 9 rises (bcc), phase2 (0x65e) spins until it falls (bcs). So the
    // bit MUST oscillate 1->0->1 as a live level, not latch. We drive it off the ARM7 cycle clock
    // (1 CPU step == 1 cycle in this fake-pipeline core) with a half-period countdown: each
    // half-period we toggle the level, firing the RTC IRQ latch on the RISING edge (so the running
    // kernel's RTC-IRQ path still works) and just dropping the level on the FALLING edge. The donor
    // instead latched bit 9 sticky (requestInterrupt(9) every 200 INT_INPUT reads); it never fell,
    // so phase2 hung forever, which is precisely why the donor NOP'd bios[0x65e]. This live square
    // wave is the faithful fix and lets the patch be deleted entirely.
    //
    // The rate MUST track RTC_MODE.0: the kernel pauses the RTC (4096 Hz) for the boot phase-align,
    // then runs it (1 Hz) for normal timekeeping. A fixed 4096 Hz floods the running kernel with an
    // RTC IRQ every ~976 cycles, which starves the GUI / COM handler (regression: the 52h read
    // underran). The RTC is a REAL-TIME oscillator independent of the CPU clock (PDA HW spec: "the
    // RTC cannot be stopped"): its rate is 1 Hz running / 4096 Hz paused REGARDLESS of the system
    // clock. tickRtc runs once per ARM7 cycle, so the half-period in CYCLES = armClockHz / (2*realHz)
    // -- only the step COUNT per RTC edge changes when CLK_MODE rescales the clock; the real-world
    // rate stays fixed. At the 4 MHz boot clock (FREQ 7) this is 3997696/8192 = 488 (paused) and
    // 3997696/2 = 1998848 (running) -- the old hardcoded constants -- and it now rescales live with
    // the software clock. TODO: calibrate the exact per-FREQ Hz against real silicon (phase 3); the
    // structural FREQ-tracking is here, exact cycle counts wait for the HW harness.
    u32 rtcCountdown = 488;     // paused 4096 Hz half-period at the 4 MHz boot clock; reset() re-seeds.
    u64 rtcEdgeCount = 0;       // RTC square-wave rising edges since reset (RUN-mode edge == 1 second).
                                // Pure instrumentation for the RTC-real-rate invariant test.

    u32 rtcHalfPeriod() const {
        const u32 realHz = (rtcMode & 1) ? 4096u : 1u;
        return armClockHz() / (2u * realHz);
    }

    // Advance the RTC square wave by one ARM7 cycle. Called once per CPU::step().
    void tickRtc() {
        if (--rtcCountdown == 0) {
            rtcCountdown = rtcHalfPeriod();
            if (irqFlags & (1u << 9)) {
                irqFlags &= ~(1u << 9);  // falling edge: drop the level only (leave the latch).
            } else {
                requestInterrupt(9);     // rising edge: raise the level AND latch the RTC IRQ.
                ++rtcEdgeCount;          // count rising edges (RUN-mode edge == 1 second; test metric).
                // In RUN mode (RTC_MODE.0=0) the square wave is 1 Hz, so this rising edge is the
                // once-per-second calendar tick: advance the BCD clock. In PAUSE mode (4096 Hz) the
                // edge is just the fast wave the boot loop phase-aligns to and must NOT advance time
                // (otherwise the kernel's brisk field-set loop would clock real seconds under it).
                if ((rtcMode & 1) == 0) rtcAdvanceOneSecond();
            }
        }
    }

    // No I/O in the constructor: just allocate the kernel/flash backing buffers so the
    // pointers the CPU page tables hold are stable. The actual contents are injected
    // later via setKernel()/setFlash(), before reset(). This is what makes the module
    // usable in-tree (the donor loaded kernel.bin/memcard2.mcd from cwd here).
    Bus(LCD& lcd) : lcd(lcd) {
        bios.resize(16 * KB, 0);
        flash.data.resize(128 * KB, 0);
    }

    // Inject the 16 KiB PocketStation kernel. Must be called before reset().
    void setKernel(const u8* data, usize len) {
        if (len != 16 * KB) {
            Helpers::panic("setKernel: expected 16 KiB, got %zu bytes", len);
        }
        std::copy(data, data + len, bios.begin());
    }

    // Inject the 128 KiB flash image (the memory-card image). Must be called before reset().
    void setFlash(const u8* data, usize len) {
        if (len != 128 * KB) {
            Helpers::panic("setFlash: expected 128 KiB, got %zu bytes", len);
        }
        std::copy(data, data + len, flash.data.begin());
    }

    std::array<u8, 128>& getVRAM() {
        return lcd.vram;
    }

    void reset();
    // Device state for savestates: everything above except the kernel image, the page tables
    // (rebuilt from the flash mappings) and the trace/diagnostic fields.
    void saveState(PSKState::Writer& w) const;
    void loadState(PSKState::Reader& r);
    void requestInterrupt(int bit);
    void releaseInterrupt(int bit);  // level drops and the pending request goes with it, no ack needed
    // Advance Timer0-2 by one system clock cycle, latching their interrupts on expiry.
    void tickTimers();

    // RTC time-of-day de-fake. seedRtcFromHostClock() initialises the BCD calendar from the host
    // wall clock at reset (so the displayed time is real); rtcAdvanceOneSecond() propagates a
    // one-second carry through seconds/minutes/hours/day-of-week/day/month/year and is called from
    // tickRtc() on each 1 Hz RTC rising edge. (Cycle-exact realtime vs host drift is expected; exact
    // rate + calendar/century fidelity wait for HW ground truth per the bootstrap order.)
    void seedRtcFromHostClock();
    void rtcAdvanceOneSecond();

    u8 read8Slow(u32 addr);
    u16 read16Slow(u32 addr);
    u32 read32Slow(u32 addr);

    void write8Slow (u32 addr, u8 val);
    void write16Slow (u32 addr, u16 val);
    void write32Slow (u32 addr, u32 val);
};