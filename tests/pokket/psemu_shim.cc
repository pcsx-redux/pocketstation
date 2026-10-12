/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
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

// The psemu_* API of mentalfoundry/Pokket (psemu/psemu.h, MIT), implemented over
// PCSX::PocketStation, so that their bus-level test suites run unmodified against this core.
// Entry points that exist only for their engine (savestates, idle skip, sub-cycle timing) call
// pokket_not_ported(), which records the case as not portable instead of failing it.

#include <cstring>
#include <vector>

#include "../../pocketstation.h"

extern "C" {
#include "harness.h"
#include "psemu/psemu.h"
}

struct psemu {
    PCSX::PocketStation::PocketStation dev;
    std::vector<uint8_t> bios = std::vector<uint8_t>(PSEMU_BIOS_SIZE, 0);
    std::vector<uint8_t> flash = std::vector<uint8_t>(PSEMU_FLASH_SIZE, 0);
    bool hasBios = false;
    int docked = 0;
    int selected = 0;
    int acked = 0;
    uint64_t time = 0;     // PSEMU_TIME_HZ units since reset
    uint64_t armAccum = 0;  // remainder, in (reference cycles * armHz) units
};

namespace {

// Advances the device by `ref` reference cycles (PSEMU_REFERENCE_CLOCK_HZ) at its live clock.
void runRef(psemu_t* ps, uint64_t ref) {
    ps->armAccum += ref * ps->dev.armClockHz();
    const uint64_t arm = ps->armAccum / PSEMU_REFERENCE_CLOCK_HZ;
    ps->armAccum -= arm * PSEMU_REFERENCE_CLOCK_HZ;
    if (arm) ps->dev.runCycles(arm);
    ps->time += ref * PSEMU_TIME_PER_REFERENCE_CYCLE;
}

constexpr uint32_t kPollChunk = 64;  // same step as Pokket's run_to_ack

}  // namespace

extern "C" {

psemu_t* psemu_create(void) { return new psemu; }
void psemu_destroy(psemu_t* ps) { delete ps; }

void psemu_reset(psemu_t* ps) {
    ps->dev.setKernel(ps->bios.data(), ps->bios.size());
    // Flash persists across a reset: take it back from the live device first.
    std::memcpy(ps->flash.data(), ps->dev.flashData(), PSEMU_FLASH_SIZE);
    ps->dev.setFlash(ps->flash.data(), ps->flash.size());
    ps->dev.reset();
    ps->time = 0;
    ps->armAccum = 0;
    ps->selected = 0;
    ps->acked = 0;
    if (ps->docked) ps->dev.setDocked(true);
}

psemu_status psemu_load_bios(psemu_t* ps, const uint8_t* data, size_t size) {
    if (size != PSEMU_BIOS_SIZE) return PSEMU_ERR_BAD_SIZE;
    std::memcpy(ps->bios.data(), data, size);
    ps->hasBios = true;
    ps->dev.setKernel(ps->bios.data(), ps->bios.size());
    return PSEMU_OK;
}

psemu_status psemu_load_flash_image(psemu_t* ps, const uint8_t* data, size_t size) {
    if (size > PSEMU_FLASH_SIZE) return PSEMU_ERR_BAD_SIZE;
    std::fill(ps->flash.begin(), ps->flash.end(), 0);
    std::memcpy(ps->flash.data(), data, size);
    ps->dev.setFlash(ps->flash.data(), ps->flash.size());
    return PSEMU_OK;
}

psemu_status psemu_load_content(psemu_t* ps, const uint8_t* data, size_t size) {
    if (size == PSEMU_FLASH_SIZE) return psemu_load_flash_image(ps, data, size);
    pokket_not_ported("psemu_load_content of an app/.mcs (no app loader in this core)");
    return PSEMU_ERR_BAD_FORMAT;
}

psemu_status psemu_load_mcs(psemu_t*, const uint8_t*, size_t) {
    pokket_not_ported("psemu_load_mcs");
    return PSEMU_ERR_BAD_FORMAT;
}

psemu_status psemu_save_flash_image(const psemu_t* ps, uint8_t* buf, size_t size) {
    if (size < PSEMU_FLASH_SIZE) return PSEMU_ERR_BAD_SIZE;
    std::memcpy(buf, ps->dev.flashData(), PSEMU_FLASH_SIZE);
    return PSEMU_OK;
}

uint8_t* psemu_flash_data(psemu_t* ps) { return ps->dev.flashDataMutable(); }
uint8_t* psemu_ram_data(psemu_t* ps) { return ps->dev.wramData(); }

uint32_t psemu_run(psemu_t* ps, uint32_t cycles) {
    runRef(ps, cycles);
    return cycles;
}

uint64_t psemu_run_time(psemu_t* ps, uint64_t units) {
    const uint64_t ref = units / PSEMU_TIME_PER_REFERENCE_CYCLE;
    runRef(ps, ref);
    return ref * PSEMU_TIME_PER_REFERENCE_CYCLE;
}

uint64_t psemu_time(const psemu_t* ps) { return ps->time; }

void psemu_set_buttons(psemu_t* ps, uint32_t buttons) { ps->dev.setButtons(buttons); }
int psemu_clk_stopped(const psemu_t* ps) { return ps->dev.halted() ? 1 : 0; }
int psemu_cpu_faulted(const psemu_t*) { return 0; }  // this core panics instead of faulting
uint32_t psemu_get_hardware_id(const psemu_t* ps) { return const_cast<psemu_t*>(ps)->dev.flashSerial(); }

int psemu_app_running(const psemu_t*) {
    pokket_not_ported("psemu_app_running");
    return 0;
}

void psemu_com_set_docked(psemu_t* ps, int docked) {
    ps->docked = docked ? 1 : 0;
    ps->dev.setDocked(docked != 0);
}
int psemu_com_get_docked(const psemu_t* ps) { return ps->docked; }

int psemu_com_is_enabled(const psemu_t* ps) {
    return (const_cast<psemu_t*>(ps)->dev.peekComFlags() & (1u << 9)) ? 1 : 0;
}

void psemu_com_set_selected(psemu_t* ps, int selected) {
    ps->selected = selected ? 1 : 0;
    ps->dev.comSetSelected(selected != 0);
}

int psemu_com_transfer_timed(psemu_t* ps, uint8_t in, uint8_t* out, uint32_t timeout, uint32_t* cyclesOut) {
    uint32_t ran = 0;
    if (!ps->selected) {
        // The PS1 selects the device ~60us before the first byte completes; the kernel's FIQ
        // starts at the select and needs that gap.
        ps->selected = 1;
        ps->dev.comSetSelected(true);
        runRef(ps, PSEMU_COM_SELECT_LEAD_CYCLES);
        ran += PSEMU_COM_SELECT_LEAD_CYCLES;
    }
    ps->dev.comBeginTransfer(in);
    uint32_t spent = 0;
    while (spent < timeout && !ps->dev.comAcked()) {
        const uint32_t step = std::min(kPollChunk, timeout - spent);
        runRef(ps, step);
        spent += step;
    }
    ran += spent;
    ps->acked = ps->dev.comAcked() ? 1 : 0;
    if (out) *out = ps->dev.comTakeReply();
    ps->dev.comEndTransfer();
    if (cyclesOut) *cyclesOut = ran;
    return ps->acked;
}

int psemu_com_transfer(psemu_t* ps, uint8_t in, uint8_t* out, uint32_t timeout) {
    return psemu_com_transfer_timed(ps, in, out, timeout, nullptr);
}

// ---- engine-specific entry points: no counterpart in this core ----------------------------
uint8_t psemu_com_send(psemu_t*, uint8_t) {
    pokket_not_ported("psemu_com_send (sub-instruction ACK timing)");
    return 0xFF;
}
uint64_t psemu_run_time_to_ack(psemu_t*, uint64_t) {
    pokket_not_ported("psemu_run_time_to_ack (sub-instruction ACK timing)");
    return 0;
}
int psemu_com_acked(const psemu_t* ps) { return ps->acked; }
int psemu_com_transfer_and_select_drop(psemu_t*, uint8_t, uint8_t*, uint32_t) {
    pokket_not_ported("psemu_com_transfer_and_select_drop");
    return 0;
}
void psemu_set_idle_skip(psemu_t*, int) { pokket_not_ported("psemu_set_idle_skip (their idle-loop skipper)"); }
size_t psemu_state_size(const psemu_t*) {
    pokket_not_ported("psemu_state_size (their savestate format)");
    return 0;
}
psemu_status psemu_save_state(const psemu_t*, void*, size_t) {
    pokket_not_ported("psemu_save_state (their savestate format)");
    return PSEMU_ERR_BAD_FORMAT;
}
psemu_status psemu_load_state(psemu_t*, const void*, size_t) {
    pokket_not_ported("psemu_load_state (their savestate format)");
    return PSEMU_ERR_BAD_FORMAT;
}
size_t psemu_snapshot_size(void) {
    pokket_not_ported("psemu_snapshot_size");
    return 0;
}
void psemu_snapshot_save(const psemu_t*, void*) { pokket_not_ported("psemu_snapshot_save"); }
psemu_status psemu_snapshot_load(psemu_t*, const void*) {
    pokket_not_ported("psemu_snapshot_load");
    return PSEMU_ERR_BAD_FORMAT;
}

}  // extern "C"
