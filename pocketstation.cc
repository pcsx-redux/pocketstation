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

#include "pocketstation.h"

#include <cstring>

namespace PCSX {
namespace PocketStation {

// Member init order: m_lcd, then m_bus(m_lcd), then m_cpu(m_bus). Declaration order in the
// header enforces this; the init list just supplies the references. The CPU ctor builds the
// ARM/Thumb LUTs and resizes the page tables -- no file I/O.
PocketStation::PocketStation() : m_bus(m_lcd), m_cpu(m_bus) {}

void PocketStation::setKernel(const uint8_t* data, size_t len) { m_bus.setKernel(data, len); }

void PocketStation::setFlash(const uint8_t* data, size_t len) { m_bus.setFlash(data, len); }

void PocketStation::reset() {
    // Donor reset order: cpu first (lays down the bios/flash page tables), then bus
    // (remaps flash banks, applies the 0x65E boot patch), then lcd.
    m_cpu.reset();
    m_bus.reset();
    m_lcd.reset();
    m_cycleCarry = 0;
    m_buttons = 0;  // bus.reset() clears irqFlags, so the reflected button levels start clear too.
}

void PocketStation::runCycles(uint64_t armCycles) {
    // Each step costs one or more system clock cycles; the overshoot of the last instruction
    // past the budget is carried into the next call so the long-run rate stays exact.
    uint64_t done = m_cycleCarry;
    while (done < armCycles) done += m_cpu.step();
    m_cycleCarry = done - armCycles;
}

void PocketStation::setButtons(uint32_t mask) {
    // Reflect the 5 buttons (bits 0..4 = Fire/Right/Left/Down/Up) into INT_INPUT as raw signal
    // LEVELS, edge-detected against the previously latched mask. A press (0->1) raises the level
    // AND latches the IRQ via requestInterrupt() (the latch only takes if the kernel has that bit
    // unmasked); a release (1->0) drops the level and the pending request. INT_ACK preserves these level bits (like
    // the dock/RTC levels), so the kernel acking a Fire IRQ does not falsely "release" a held key.
    //
    // Doing this on the press edge, which runs while the core is halted, is what lets a
    // Fire press wake a sleeping (CLK_STOP) device: the press latches IRQ-0, satisfying CPU::step()'s
    // wake condition (irqLatch & irqMask). psx-spx: buttons are polled directly from INT_INPUT,
    // except in Sleep mode where the Fire-button IRQ wakes the device. Edge-detecting against
    // m_buttons keeps a per-frame setButtons(sameMask) a no-op, so a held key can't re-latch / storm.
    const uint32_t changed = (mask ^ m_buttons) & 0x1F;
    for (int key = 0; key < 5; key++) {
        const uint32_t bit = 1u << key;
        if (!(changed & bit)) continue;
        if (mask & bit) {
            m_bus.requestInterrupt(key);  // press edge: raise level + latch (if unmasked).
        } else {
            m_bus.releaseInterrupt(key);  // release edge: drop the level and its pending request.
        }
    }
    m_buttons = mask;
}

// Blob layout: "PSKS", a format version, then the bus, CPU, LCD and device fields in that order.
static constexpr uint8_t kStateMagic[4] = {'P', 'S', 'K', 'S'};
static constexpr uint32_t kStateVersion = 2;

std::vector<uint8_t> PocketStation::saveState() const {
    PSKState::Writer w;
    w.bytes(kStateMagic, sizeof(kStateMagic));
    w.u32v(kStateVersion);
    m_bus.saveState(w);
    m_cpu.saveState(w);
    m_lcd.saveState(w);
    w.u32v(m_buttons);
    w.u64v(m_cycleCarry);
    return std::move(w.buf);
}

bool PocketStation::loadState(const uint8_t* data, size_t len) {
    // Every field is fixed-width, so a blob of this version is exactly as long as one written now.
    if (len != saveState().size() || std::memcmp(data, kStateMagic, sizeof(kStateMagic)) != 0) return false;
    PSKState::Reader r{data + sizeof(kStateMagic), len - sizeof(kStateMagic)};
    if (r.u32v() != kStateVersion) return false;
    // Lay down the fixed page tables (BIOS, physical flash) before the state goes over them.
    m_cpu.reset();
    m_bus.reset();
    m_bus.loadState(r);
    m_cpu.loadState(r);  // after the bus: its WRAM-over-BIOS flag edits page 0 last
    m_lcd.loadState(r);
    m_buttons = r.u32v();
    m_cycleCarry = r.u64v();
    return r.ok && r.left == 0;
}

const uint8_t* PocketStation::vram() const { return m_lcd.vram.data(); }

uint32_t PocketStation::lcdMode() const { return m_lcd.mode.raw; }

bool PocketStation::lcdEnabled() const { return m_lcd.mode.enabled; }

}  // namespace PocketStation
}  // namespace PCSX
