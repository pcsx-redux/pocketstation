#include "cpu.h"
#include "io.h"

void CPU::write8 (u32 addr, u8 val) {
    cycles += dataCycles(addr);
    const auto page = addr >> 11;
    const auto pointer = writeTable[page];

    if (pointer == 0) { // if the address is a slow path
        if (addr == IO::F_CTRL && val == 3) { // Writing 3 to F_CTRL memory-maps SRAM
            const auto pointer = (uintptr_t)&bus.wram[0];
            readTable[0] = pointer;
            writeTable[0] = pointer;
        } else {
            Helpers::panic ("ARM7 slowmem write. Wrote %02X to %08X\nPage: %06X", val, addr, page);
        }
    }

    else 
        *(u8*) (pointer + (addr & 0x7ff)) = val;
}

void CPU::write16 (u32 addr, u16 val) {
    cycles += dataCycles(addr);
    const auto page = addr >> 11;
    const auto pointer = writeTable[page];

    if (pointer == 0) { // if the address is a slow path
        bus.write16Slow(addr, val);
    }

    else
        *(u16*) (pointer + (addr & 0x7ff)) = val;
}

void CPU::write32 (u32 addr, u32 val) {
    cycles += dataCycles(addr);
    const auto page = addr >> 11;
    const auto pointer = writeTable[page];

    if (pointer == 0) { // if the address is a slow path
        bus.write32Slow(addr, val);
    }

    else 
        *(u32*) (pointer + (addr & 0x7ff)) = val;
}

u8 CPU::read8 (u32 addr) {
    cycles += dataCycles(addr);
    const auto page = addr >> 11;
    const auto pointer = readTable[page];

    if (pointer == 0) // if the address is a slow path
        Helpers::panic ("ARM7: Read byte from unknown address %08X\nPage in fastmem: %06X\n", addr, page);
    else 
        return *(u8*) (pointer + (addr & 0x7ff));
}

u16 CPU::read16 (u32 addr) {
    cycles += dataCycles(addr);
    return (u16)read16Raw(addr);
}

// Uncosted 16-bit read: the opcode fetch path (executeThumb charges fetchCycles instead).
u32 CPU::read16Raw (u32 addr) {
    const auto page = addr >> 11;
    const auto pointer = readTable[page];

    if (pointer == 0) { // if the address is a slow path
        return bus.read16Slow(addr);
    }

    else 
        return *(u16*) (pointer + (addr & 0x7ff));
}

u32 CPU::read32 (u32 addr) {
    cycles += dataCycles(addr);
    return read32Raw(addr);
}

// Uncosted 32-bit read: the opcode fetch path (executeARM charges fetchCycles instead).
u32 CPU::read32Raw (u32 addr) {
    const auto page = addr >> 11;
    const auto pointer = readTable[page];

    if (pointer == 0) {// if the address is a slow path
        return bus.read32Slow(addr);
    }
    
    else 
        return *(u32*) (pointer + (addr & 0x7ff));
}

void CPU::pollInterrupts() {
    // Dispatch off the LATCH, not the raw INT_INPUT level. The PocketStation interrupts are
    // edge-triggered (psx-spx: request bits set on a 0-to-1 transition) and cleared via INT_ACK;
    // INT_INPUT mirrors the raw signal LEVELS (readable, but must not re-trigger). The donor
    // dispatched off (irqMask & irqFlags) = the raw level, which works for self-clearing sources
    // (timers) but STORMS on a stuck level: INT_INPUT.11 (Docked) stays high while docked and
    // INT_ACK deliberately preserves it, so a level-based dispatch re-enters IRQ-11 every
    // instruction and starves the GUI (so SWI 05h never sets ComFlags.9 / enables COM). Using the
    // ack-clearable latch matches the hardware and fixes the storm. (Verified 2026-06-02.)
    const auto interrupts = bus.irqLatch & bus.irqMask;
    if (likely(interrupts == 0)) return;

    // Check if an FIQ has is to be fired, otherwise check if an IRQ is to be fired
    if (bus.comTrace) {
        if (!cpsr.fiqDisable && (interrupts & 0x2040))
            printf("[PSK] DISPATCH FIQ bits=%08X (mask=%08X)\n", interrupts & 0x2040, bus.irqMask);
        else if (!cpsr.irqDisable && (interrupts & 0x1F9F))
            printf("[PSK] DISPATCH IRQ bits=%08X (mask=%08X)\n", interrupts & 0x1F9F, bus.irqMask);
    }
    if (!cpsr.fiqDisable && (interrupts & 0x2040)) {
        const auto lr = cpsr.thumb ? registers[15] : registers[15] - 4;

        const auto newSPSR = cpsr.raw;
        setMode(CPUModes::FIQ);

        spsr.raw = newSPSR;
        cpsr.thumb = 0; // Exit thumb state
        cpsr.irqDisable = 1; // Disable IRQs
        cpsr.fiqDisable = 1; // Disable FIQs
        registers[14] = lr; // Set return address
        registers[15] = 0x1C; // Jump to FIQ vector

        registers[15] += 8; // Fake pipeline
        cycles += 2 * fetchCycles(0x1C, false);  // exception entry = pipeline refill at the vector
    } else if (!cpsr.irqDisable && (interrupts & 0x1F9F)) {
        const auto lr = cpsr.thumb ? registers[15] : registers[15] - 4;

        const auto newSPSR = cpsr.raw;
        setMode(CPUModes::IRQ);

        spsr.raw = newSPSR;
        cpsr.thumb = 0; // Exit thumb state
        cpsr.irqDisable = 1; // Disable IRQs
        registers[14] = lr; // Set return address
        registers[15] = 0x18; // Jump to IRQ vector

        registers[15] += 8; // Fake pipeline
        cycles += 2 * fetchCycles(0x18, false);  // exception entry = pipeline refill at the vector
    }
}

u32 CPU::step() {
    bool woke = false;  // the wake cycle ticked the RTC above, so it counts toward this step
    if (bus.halted) {
        // CLK_STOP sleep: the CPU core clock is stopped. Advance the RTC oscillator (it keeps
        // running and its square wave can wake the core) but execute NO instruction this cycle.
        // Wake when an unmasked interrupt has latched (irqLatch & irqMask) -- the same condition
        // pollInterrupts dispatches on. On wake, fall through so pollInterrupts services it and
        // execution resumes at the instruction after the CLK_STOP store. (psx-spx: "Stops the CPU
        // until an interrupt occurs"; wakeup via IRQ-0 Fire / IRQ-9 RTC / IRQ-11 Docking.)
        bus.tickRtc();
        if (bus.irqLatch & bus.irqMask) {
            bus.halted = false;
            woke = true;
        } else {
            return 1;  // still asleep: no instruction executed (cheap idle, ~no host CPU burned).
        }
    }

    cycles = 0;
    branchTaken = false;
    pollInterrupts();  // charges the exception-entry refill if it dispatches

    const u32 pcBefore = registers[15];
    const bool thumbBefore = cpsr.thumb;
    if (!thumbBefore)
        executeARM();
    else
        executeThumb();

    // Any non-sequential PC (taken branch, BX, load/ALU into PC, SWI, a state switch) refills the
    // pipeline: two more fetches at the new PC, in the new state. The branch handlers flag a taken
    // branch explicitly, since one whose target is the next instruction leaves the PC sequential.
    const u32 seqPC = pcBefore + (thumbBefore ? 2 : 4);
    if (branchTaken || registers[15] != seqPC || cpsr.thumb != thumbBefore) {
        const u32 target = registers[15] - (cpsr.thumb ? 4 : 8);
        cycles += 2 * fetchCycles(target, cpsr.thumb);
    }

    // The timers and the RTC run off the same system clock: advance them by this instruction's
    // cost. (A CLK_STOP store sets bus.halted mid-instruction; its own cycles still elapse.)
    for (u32 i = 0; i < cycles; i++) {
        bus.tickRtc();
        bus.tickTimers();
    }

    instructions++;
    return cycles + (woke ? 1 : 0);
}

void CPU::executeARM() {
    if (bus.comTrace) bus.curPC = registers[15] - 8;  // stash fetch PC for COM/INT access tracing.
    const auto instruction = read32Raw (registers[15] - 8);
    cycles += fetchCycles(registers[15] - 8, false);
    //printf("Hello. PC: %08X. Instruction %08X\n", registers[15] - 8, instruction);

    if (isConditionTrue(instruction >> 28)) {
        cycles += armInternalCycles(instruction);  // before execution: MUL reads Rs before writeback
        const auto lutIndex = ((instruction >> 4) & 0xF) | ((instruction >> 16) & 0xFF0); // compute a hash to index into the function pointer table
        (*this.*ARM_LUT[lutIndex]) (instruction); // call function
    }

    else {
        registers[15] += 4;
        if ((instruction >> 28) == 0xF)
            Helpers::panic ("Condition code NV");
    }
}

void CPU::executeThumb() {
    if (bus.comTrace) bus.curPC = registers[15] - 4;  // stash fetch PC for COM/INT access tracing.
    const auto instruction = (u16)read16Raw (registers[15] - 4);
    cycles += fetchCycles(registers[15] - 4, true);
    cycles += thumbInternalCycles(instruction);  // before execution: MUL reads Rs before writeback
    //printf("Hello. PC: %08X. Instruction: %04X\n", registers[15] - 4, instruction);
    const auto index = instruction >> 8;
    (*this.*Thumb_LUT[index]) (instruction);
}

// Internal (I) cycles of a condition-passed ARM instruction (ARM7TDMI TRM ch. 7). The fetch, data
// accesses and any refill are charged elsewhere. Screen 1 (LDR loop) and screen 3 (BIOS ARM helper:
// LDRB) pin the load +1I; the rest is the documented core timing.
u32 CPU::armInternalCycles(u32 i) const {
    switch ((i >> 25) & 7) {
        case 0:
            if ((i & 0x90) == 0x90) {
                if ((i & 0x60) == 0) {
                    const u32 m = mulM(registers[(i >> 8) & 0xF]);
                    const u32 acc = (i >> 21) & 1;
                    if ((i & 0x0FC000F0) == 0x00000090) return m + acc;      // MUL/MLA: m (+1)
                    if ((i & 0x0F8000F0) == 0x00800090) return m + 1 + acc;  // [US]MULL/MLAL
                    if ((i & 0x0FB00FF0) == 0x01000090) return 1;            // SWP/SWPB: +1I
                    return 0;
                }
                return (i >> 20) & 1;  // LDRH/LDRSB/LDRSH: +1I, STRH: none
            }
            if ((i & 0x0FFFFFF0) == 0x012FFF10) return 0;  // BX
            return (i >> 4) & 1;  // data processing with a register-specified shift: +1I
        case 2:
        case 3:  // LDR/LDRB: +1I
        case 4:  // LDM: +1I
            return (i >> 20) & 1;
        default:
            return 0;
    }
}

// Internal (I) cycles of a Thumb instruction. Screen 1 (Thumb LDR loop) and screen 4 (BIOS Thumb
// helper) pin the load +1I.
u32 CPU::thumbInternalCycles(u32 i) const {
    switch (i >> 12) {
        case 0x4:
            if ((i & 0xFC00) == 0x4000) {  // ALU ops
                const u32 op = (i >> 6) & 0xF;
                if (op == 2 || op == 3 || op == 4 || op == 7) return 1;  // LSL/LSR/ASR/ROR by reg
                if (op == 13) return mulM(registers[(i >> 3) & 7]);     // MUL: m
                return 0;
            }
            return (i & 0xF800) == 0x4800 ? 1 : 0;  // LDR PC-relative
        case 0x5:
            return ((i >> 9) & 7) >= 3 ? 1 : 0;  // register offset: LDSB/LDR/LDRH/LDRB/LDSH
        case 0x6:
        case 0x7:
        case 0x8:
        case 0x9:  // immediate / SP-relative loads
        case 0xC:  // LDMIA
            return (i >> 11) & 1;
        case 0xB:
            return (i & 0x0600) == 0x0400 ? (i >> 11) & 1 : 0;  // POP: +1I
        default:
            return 0;
    }
}

// fake pipeline for speed
// self modifying code is not handled yet, it causes a crash with a warning
// prolly will fix later on
void CPU::refillPipeline() {
    if (cpsr.thumb)
        registers[15] += 4;

    else
        registers[15] += 8;
}

void CPU::handleUndefined (u32 instruction) { // handle undefined/unknown instruction
    Helpers::panic("Unknown %s mode instruction: %08X\nAddress: %08X", cpsr.thumb ? "Thumb" : "ARM", instruction, cpsr.thumb ? registers[15]-4 : registers[15]-8);
}

void CPU::setNZ (u32 number) { // set the N and Z flags depending on the number
    cpsr.negative = number >> 31;
    cpsr.zero = number == 0;
}
    
bool CPU::isConditionTrue (int cond) {
    return COND_TABLE[cond] & (1 << (cpsr.raw >> 28)); // fast bit magic for condition checking
}

void CPU::setCPSR (u32 val) {
    setMode((CPUModes) (val & 0x1F));
    cpsr.raw = val;
}

void CPU::setMode (CPUModes mode) {
    if (cpsr.mode == mode) return;
    auto modeToIndex = [](CPUModes mode) {
        switch (mode) {
            case User:
            case System:
                return 0;
            case FIQ: return 1;
            case IRQ: return 2;
            case SVC: return 3;
            case ABT: return 4;
            case UND: return 5;
            default: Helpers::panic ("Invalid PSR mode: %d\n", static_cast<int>(mode));
        }
    };

    switch (cpsr.mode) { // store current regs
        case FIQ:
            r8_banks[1] = registers[8];
            r9_banks[1] = registers[9];
            r10_banks[1] = registers[10];
            r11_banks[1] = registers[11];
            r12_banks[1] = registers[12];
            r13_banks[1] = registers[13];
            r14_banks[1] = registers[14];
            spsr_banks[0] = spsr;
            break;

        case User:
        case System:
            r8_banks[0] = registers[8];
            r9_banks[0] = registers[9];
            r10_banks[0] = registers[10];
            r11_banks[0] = registers[11];
            r12_banks[0] = registers[12];
            r13_banks[0] = registers[13];
            r14_banks[0] = registers[14];
            break;
        
        default: {
            auto index = modeToIndex((CPUModes) cpsr.mode);
            r8_banks[0] = registers[8];
            r9_banks[0] = registers[9];
            r10_banks[0] = registers[10];
            r11_banks[0] = registers[11];
            r12_banks[0] = registers[12];
            r13_banks[index] = registers[13];
            r14_banks[index] = registers[14];
            spsr_banks[index-1] = spsr;
        }
    }

    switch (mode) { // load new registers
        case FIQ:
            registers[8] = r8_banks[1];
            registers[9] = r9_banks[1];
            registers[10] = r10_banks[1];
            registers[11] = r11_banks[1];
            registers[12] = r12_banks[1];
            registers[13] = r13_banks[1];
            registers[14] = r14_banks[1];
            spsr_banks[0] = spsr;
            break;

        case User:
        case System:
            registers[8] = r8_banks[0];
            registers[9] = r9_banks[0];
            registers[10] = r10_banks[0];
            registers[11] = r11_banks[0];
            registers[12] = r12_banks[0];
            registers[13] = r13_banks[0];
            registers[14] = r14_banks[0];
            break;
        
        default: {
            auto index = modeToIndex (mode);
            registers[8] = r8_banks[0];
            registers[9] = r9_banks[0];
            registers[10] = r10_banks[0];
            registers[11] = r11_banks[0];
            registers[12] = r12_banks[0];
            registers[13] = r13_banks[index];
            registers[14] = r14_banks[index];
            spsr = spsr_banks[index-1];
        }
    }

    cpsr.mode = mode;
}