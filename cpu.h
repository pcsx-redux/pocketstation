#pragma once
#include <bit>
#include <array>
#include <cassert>
#include "types.h"
#include "bus.h"

enum CPUModes {
    User = 0x10,
    FIQ  = 0x11,
    IRQ  = 0x12,
    SVC  = 0x13,
    ABT  = 0x17,
    UND  = 0x1B,
    System  = 0x1F
};

enum ARMInstrTypes {
    DataProcessing,
    Branch,
    BX, // Stupid instructions that don't want to fit in with the rest
    SWI,
    Multiply,
    MultiplyLong,
    PSRTransfer,
    Swap,
    LoadStoreWord, // load store byte/word
    LoadStoreMisc, // load store halfword/doubleword/sign extended value
    LoadStoreMultiple,
    UndefinedInstruction
};

union PSR {
    u32 raw;
    struct {
        unsigned mode: 5; // Current CPU mode
        unsigned thumb: 1; // is in thumb mode?
        unsigned fiqDisable: 1; // are FIQs disabled?
        unsigned irqDisable: 1; // are IRQs disabled?
        unsigned reserved: 19;

        unsigned saturation: 1; // saturation flag (Does not exist on v5)
        unsigned overflow: 1;   // overflow flag
        unsigned carry: 1;      // carry flag
        unsigned zero: 1;       // zero flag
        unsigned negative: 1;   // negative flag
    };
};
static_assert(sizeof(PSR) == 4);

class CPU {
    typedef void (CPU::*InstructionCallback) (u32 instruction); // function pointer type for instructions
    std::array <u32, 16> registers;
    PSR cpsr;
    PSR spsr;
    Bus& bus;

    std::array<u32, 2> r8_banks;
    std::array<u32, 2> r9_banks;
    std::array<u32, 2> r10_banks;
    std::array<u32, 2> r11_banks;
    std::array<u32, 2> r12_banks;
    std::array<u32, 6> r13_banks;
    std::array<u32, 6> r14_banks;
    std::array<PSR, 5> spsr_banks;

    std::array<u32, 3> pipeline; // ARM pipeline

    static constexpr u32 pageSize = 1 << 11;
    
    // Page tables used for software fastmem. These are copies of Bus::readTable and Bus::writeTable
    uintptr_t *readTable, *writeTable;

    /// The CPU core uses function pointer tables instead of fully decoding instructions on the fly
    std::array<InstructionCallback, 4096> ARM_LUT;
    std::array<InstructionCallback, 256> Thumb_LUT;
    
    static constexpr std::array <u16, 16> COND_TABLE = { // table of masks used for evaluating instruction codes
        0xF0F0, // EQ (z)
        0x0F0F, // NE (!z)
        0xCCCC, // CS (c)
        0x3333, // CC (!c)
        0xFF00, // MI (n)
        0x00FF, // PL (!n)
        0xAAAA, // VS (v)
        0x5555, // VC (!v)
        0x0C0C, // HI (c && !z)
        0xF3F3, // LS (!c || z)
        0xAA55, // GE (n == v)
        0x55AA, // LT (n != v)
        0x0A05, // GT (!z && n == v)
        0xF5FA, // LE (z || n != v)
        0xFFFF, // AL (always)
        0x0000, // NV (never, used for special instructions in v5+)
    };

    void refillPipeline();
    void advancePipeline();

    void executeARM();
    void executeThumb();
    bool isConditionTrue(int cond);
    void setCPSR(u32 val);
    void setMode(CPUModes mode);
    void setNZ(u32 number);

    // inline files for instruction handlers (these get thrown into inline files)
    #include "cpu/helpers.inl"
    #include "cpu/arm/branch.inl"
    #include "cpu/arm/dataProcessing.inl"
    #include "cpu/arm/loadsStores.inl"
    #include "cpu/arm/loadStoreMultiple.inl"
    #include "cpu/arm/multiplication.inl"
    #include "cpu/arm/psrTransfers.inl"

    #include "cpu/thumb/dataProcessing.inl"
    #include "cpu/thumb/branch.inl"
    #include "cpu/thumb/loadStoreMultiple.inl"
    #include "cpu/thumb/loadsStores.inl"

    // code for table generation here
    #include "cpu/tablegen.inl" 

    void handleUndefined(u32 instruction);
    void pollInterrupts();

    // ---- Per-instruction cycle cost ------------------------------------------------------------
    // The ARM7TDMI S/N/I cycle model with the PocketStation's per-region wait states, as measured
    // on a retail unit by mentalfoundry's pk_timing_bench (Pokket pk_timing_bench/VERIFICATION.md;
    // the same table as Pokket core/src/memory.c). One instruction costs:
    //   fetch(PC region, ARM/Thumb)                       every instruction, executed or not
    // + data(addr region) per load/store access            each CPU::read*/write* call, any width
    // + internal (I) cycles                                LDR/LDM/SWP +1, reg-specified shift +1, MUL m
    // + 2 * fetch(new PC region, new state)                pipeline refill on any non-sequential PC
    //                                                      (B/BL/BX, LDR/LDM/POP/ALU into PC, SWI)
    // + 2 * fetch(vector, ARM)                             IRQ/FIQ entry
    // CPU::step() then advances the timers and the RTC by that many system clock cycles.
    struct RegionCost {
        u8 data;        // cycles per data access (any width, sequential or not)
        u8 fetchArm;    // cycles per 32-bit ARM opcode fetch
        u8 fetchThumb;  // cycles per 16-bit Thumb opcode fetch
    };
    // Indexed by address bits 27-24. The pk_timing_bench screen that pins each value:
    //  WRAM    data 1 / fetch 1: screen 9 control (WRAM STR loop, 11 cycles/iter = 0x2849), screen 3/4
    //          controls (helper copies run from WRAM), screen 1 (WRAM LDR in both loops).
    //  FLASH   data 2, fetch ARM 2 / Thumb 1: screen 1 (the same loop run from FLASH: ARM 12 cycles/iter
    //          = 0x2BF2, Thumb 7 = 0x19A3), and the loop overhead of screens 2, 7, 9.
    //  BIOS    fetch ARM 2 / Thumb 1, as FLASH: screens 3 (ARM helper 0x802D vs its WRAM copy 0x6A33)
    //          and 4 (Thumb helper 0xB71B vs its WRAM copy 0xB371: Thumb fetches cost the same as
    //          WRAM; the 1 cycle/iteration gap is the helper's PC-relative literal load, a BIOS
    //          data access at 2 cycles).
    //  F_CTRL  data 1 (the WRAM rate): screen 2 (FLASH_CTRL+0x100 LDR loop == the WRAM loop, 0x2BF2).
    //  I/O     data 2: screen 9 test (IRDA_DATA STR loop 12 cycles/iter = 0x2BF2 vs WRAM 0x2849),
    //          and the Timer0/INT register accesses of screen 7's interrupt path.
    // WRAM is only the 2 KiB at 0x0..0x7FF; dataCycles/fetchCycles route the rest of region 0 to the
    // slow default. Region 0x0 maps BIOS until F_CTRL=3, which is charged as WRAM (boot only).
    static constexpr RegionCost kSlowRegion = {2, 2, 2};
    static constexpr std::array<RegionCost, 16> kRegionCost = {{
        {1, 1, 1},  // 0x0: WRAM
        kSlowRegion,
        {2, 2, 1},  // 0x2: FLASH1 (banked virtual window)
        kSlowRegion,
        {2, 2, 1},  // 0x4: BIOS ROM
        kSlowRegion,
        {1, 2, 2},  // 0x6: FLASH_CTRL registers (data at the WRAM rate; nothing executes here)
        kSlowRegion,
        {2, 2, 1},  // 0x8: FLASH2 (physical)
        kSlowRegion,
        kSlowRegion,  // 0xA: interrupt controller, timers
        kSlowRegion,  // 0xB: CLK, RTC
        kSlowRegion,  // 0xC: COM, IrDA
        kSlowRegion,  // 0xD: LCD, IOP, DAC
        kSlowRegion,
        kSlowRegion,
    }};
    static const RegionCost& regionCost(u32 addr) {
        if (addr < 0x800) return kRegionCost[0];
        if (addr < 0x01000000 || addr >= 0x10000000) return kSlowRegion;
        return kRegionCost[addr >> 24];
    }
    static u32 dataCycles(u32 addr) { return regionCost(addr).data; }
    static u32 fetchCycles(u32 addr, bool thumb) {
        return thumb ? regionCost(addr).fetchThumb : regionCost(addr).fetchArm;
    }
    // Multiply early termination (ARM7TDMI TRM): m = 1..4 by how many top bytes of the multiplier
    // are all zeros or all ones. No bench screen constrains it; it is the documented core behaviour.
    static u32 mulM(u32 rs) {
        if ((rs >> 8) == 0 || (rs >> 8) == 0x00FFFFFF) return 1;
        if ((rs >> 16) == 0 || (rs >> 16) == 0xFFFF) return 2;
        if ((rs >> 24) == 0 || (rs >> 24) == 0xFF) return 3;
        return 4;
    }
    u32 armInternalCycles(u32 instruction) const;
    u32 thumbInternalCycles(u32 instruction) const;
    u32 read16Raw(u32 addr);
    u32 read32Raw(u32 addr);

    u32 cycles = 0;  // cycles charged so far to the instruction CPU::step() is executing
    // Set by the branch handlers (B, BL, BX, Thumb B/Bcond/BL): a taken branch always refills the
    // pipeline, even when its target is the next instruction and the PC looks sequential.
    // pk_timing_bench screen 4's helper ends in such a BNE (0xD1FF): 2 cycles/iteration.
    bool branchTaken = false;

    u64 instructions = 0;  // count of instructions actually executed (not advanced while halted)

public:
    // Instructions executed since reset. Stays flat while the core is halted (CLK_STOP sleep),
    // so the gap between two samples proves cheap idle (a handful per wake, not ~4M/second).
    u64 instructionCount() const { return instructions; }

    void log() {
        printf("PC: %08X\n", registers[15]);
    }
    void reset() {
        instructions = 0;
        readTable = bus.readTable.data();
        writeTable = bus.writeTable.data();

        registers[15] = 0;
        cpsr.raw = 0x5F;
        spsr.raw = 0x5F;

        // On reset, the BIOS is mapped over RAM until the BIOS writes 3 to F_CTRL
        readTable[0] = (uintptr_t)&bus.bios[0];
        u32 page = 0x4000000 / pageSize;

        // Map the 16 KiB (8 pages) of BIOS memory as read-only
        for (u32 i = 0; i < 8; i++) {
            const auto pointer = (uintptr_t)&bus.bios[i * pageSize];
            readTable[page++] = pointer;
        }

        page = 0x8000000 / pageSize;
        // Map the 128 KiB (64 pages) of FLASH as readable/executable only. Flash is NOT directly
        // writable: real NOR flash needs the FLASHDataController program sequence (JEDEC unlock +
        // data phase), so writes are routed to the slow path (Bus::flashProgramWrite) instead of
        // landing via fastmem. A plain `str` to flash therefore does nothing, as on hardware.
        for (u32 i = 0; i < 64; i++) {
            readTable[page++] = (uintptr_t)&bus.flash.data[i * pageSize];
        }

        setCPSR(0x5F); // initialize CPSR/SPSR
        refillPipeline();
    }

    CPU(Bus& bus) : bus(bus) {
        // initialize the instruction LUTs
        populateARMTable();
        populateThumbTable();
        bus.readTable.resize(0x200000, 0);
        bus.writeTable.resize(0x200000, 0);
    }

    // Register file for savestates: the live registers, CPSR/SPSR, every banked copy, the
    // instruction count, and whether F_CTRL=3 has mapped WRAM over the BIOS at address 0. Load
    // after Bus::loadState(), on a core that has been reset() once (the BIOS/flash page tables).
    void saveState(PSKState::Writer& w) const;
    void loadState(PSKState::Reader& r);

    void write8(u32 addr, u8 val);
    void write16(u32 addr, u16 val);
    void write32(u32 addr, u32 val);
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);

    // Execute one instruction (or one cycle of CLK_STOP sleep) and advance the timers and RTC by
    // its cost. Returns the number of system clock cycles consumed (>= 1).
    u32 step();
};