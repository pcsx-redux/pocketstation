/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "bus.h"
#include "cpu.h"
#include "lcd.h"

namespace PCSX {
namespace PocketStation {

// Headless PocketStation device. Owns the ARM7 core, the bus (kernel + flash + MMIO),
// and the LCD. No I/O happens in construction; the kernel and flash images are injected
// before reset(). This replaces the donor's Qt-bound Pocketstation class.
//
// The catch-up entry point is runCycles(): the host (a Redux scheduler event) advances
// the ARM7 by a cycle delta, and the device steps the core that far. The LCD framebuffer
// is read out via vram()/lcdMode()/lcdEnabled().
class PocketStation {
  public:
    PocketStation();  // builds the ARM/Thumb LUTs; no I/O.

    void setKernel(const uint8_t* data, size_t len);  // 16 KiB; call before reset().
    void setFlash(const uint8_t* data, size_t len);   // 128 KiB card image; call before reset().
    const uint8_t* flashData() const { return m_bus.flash.data.data(); }  // read flash back (verify writes).
    size_t flashSize() const { return m_bus.flash.data.size(); }

    void reset();  // power-on; donor reset order (cpu, bus, lcd).

    void runCycles(uint64_t armCycles);  // step the ARM core forward N cycles.

    const uint8_t* vram() const;  // 128 bytes, 32x32 @ 1bpp.
    uint32_t lcdMode() const;
    bool lcdEnabled() const;

    void setButtons(uint32_t mask);  // bits 0..4 -> INT_INPUT.0..4 (live levels).

    // ---- system clock (CLK_MODE.FREQ) -------------------------------------------------------
    // Live ARM7 system clock in Hz, from the current CLK_MODE.FREQ. The SIO cycle catch-up reads
    // this per device so the PSX-cycle / wall-clock -> ARM7-cycle scale tracks a software clock
    // change (SWI 4 "Set system clock frequency").
    uint32_t armClockHz() const { return m_bus.armClockHz(); }
    // Test-only: force CLK_MODE.FREQ (white-box) to drive the RTC-real-rate invariant from a harness.
    void setClockForTest(uint32_t freq) { m_bus.setClkModeForTest(freq); }
    // Test-only: RTC square-wave rising edges since reset (RUN-mode edge == 1 second). Lets a harness
    // measure ARM7 steps per RTC tick at different clocks to prove the real rate stays 1 Hz.
    uint64_t rtcEdges() const { return m_bus.rtcEdgeCount; }

    // ---- COM link (PS1 card-slot bridge) ----------------------------------------------------
    // The PS1 side of the card link, per exchange: hold /SEL (comSetSelected(true)) at the start of
    // a command and let the ARM7 run before the first byte (the kernel's FIQ starts at the select);
    // comBeginTransfer(byte); run the ARM7 until comAcked() or a timeout (no acknowledge = no card,
    // or the last byte of a command); comTakeReply() is the byte that went out during that exchange
    // (the reply the kernel loaded one exchange earlier); comEndTransfer(). Release /SEL
    // (comSetSelected(false)) after the command and run the ARM7 so the kernel leaves its
    // end-of-command wait.
    void comSetSelected(bool selected) { m_bus.comSetSelected(selected); }
    void comBeginTransfer(uint8_t in) { m_bus.comBeginTransfer(in); }
    bool comAcked() const { return m_bus.comAcked(); }
    uint8_t comTakeReply() const { return m_bus.comTakeReply(); }
    void comEndTransfer() { m_bus.comEndTransfer(); }
    // Shorthand for select-if-needed + comBeginTransfer; returns comTakeReply().
    uint8_t comExchange(uint8_t in) { return m_bus.comExchange(in); }
    // /SEL released.
    void comDeselect() { m_bus.comDeselect(); }
    // Dock state: drives INT_INPUT.11 and fires IRQ-11 on a transition (kernel enables COM).
    void setDocked(bool docked) { m_bus.setDocked(docked); }

    // ---- sleep / idle instrumentation -------------------------------------------------------
    // Instructions actually executed since reset (flat while halted). Sample across an interval to
    // prove cheap idle: a sleeping device runs a handful of instructions per wake, not ~4M/second.
    uint64_t instructionsExecuted() const { return m_cpu.instructionCount(); }
    // Whether the core is currently parked in CLK_STOP sleep (battery/standby).
    bool halted() const { return m_bus.halted; }

    void setComTrace(bool on) { m_bus.comTrace = on; }   // log every COM/INT reg access with PC.
    uint32_t comUnderruns() const { return m_bus.com.hostUnderruns; }  // exchanges with no fresh reply loaded.

    // Diagnostics for reversing the docking/COM-enable handshake.
    uint32_t peekComFlags() const { return *reinterpret_cast<const uint32_t*>(&m_bus.wram[0xC0]); }
    uint32_t peekIrqMask() const { return m_bus.irqMask; }
    uint32_t peekIntInput() const { return m_bus.irqFlags; }

    // ---- test harness access ----------------------------------------------------------------
    uint8_t* wramData() { return m_bus.wram.data(); }  // 2 KiB work RAM (kernel state at 0x00-0x1FF).
    uint8_t* flashDataMutable() { return m_bus.flash.data.data(); }
    uint32_t flashSerial() { return (uint32_t(m_bus.read16Slow(0x06000302)) << 16) | m_bus.read16Slow(0x06000300); }

  private:
    // Init order matters: LCD first, then Bus(lcd), then CPU(bus). Members are constructed
    // in declaration order, so this declaration order IS the construction contract.
    ::LCD m_lcd;
    ::Bus m_bus;
    ::CPU m_cpu;

    uint32_t m_buttons = 0;  // latched button mask, reflected into INT bits 0..4 per frame.
    uint64_t m_cycleCarry = 0;  // cycles the last runCycles() step overran its budget by.
};

}  // namespace PocketStation
}  // namespace PCSX
