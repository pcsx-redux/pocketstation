
template <bool affectFlags>
u32 inline LSL (u32 number, u32 amount) {
    if (amount < 32) {
        if constexpr (affectFlags) {
            if (amount != 0)
                cpsr.carry = (number >> (32 - amount)) & 1; // set carry to the last bit shifted out
        }

        return number << amount;
    }

    else {
        if constexpr (affectFlags) {
            if (amount == 32) // Amount == 32 is a special case that returns the LSB of the shifted number. Thanks ARM
                cpsr.carry = number & 1;
            else // If amount > 32, carry always gets cleared
                cpsr.carry = 0;
        }

        return 0;
    }
}

template <bool affectFlags>
u32 inline LSR (u32 number, u32 amount) {
    if (amount < 32) {
        if constexpr (affectFlags) {
            if (amount != 0)
                cpsr.carry = (number >> (amount-1)) & 1; // set carry to the last bit shifted out
        }

        return number >> amount;
    }

    else {
        if constexpr (affectFlags) {
            if (amount == 32) // Amount == 32 is a special case that returns the MSB of the shifted number. Thanks ARM
                cpsr.carry = number >> 31;
            else // If amount > 32, carry always gets cleared
                cpsr.carry = 0;
        }

        return 0;
    }
}

template <bool affectFlags>
u32 inline ASR (u32 number, u32 amount) {
    if (amount < 32) {
        if constexpr (affectFlags) {
            if (amount != 0)
                cpsr.carry = (number >> (amount-1)) & 1; // set carry to the last bit shifted out
        }
       
        return ((s32) number) >> amount; // cast to s32 so that the compiler will interpret this as an arithmetic shift, not a logical one
    }

    else { // amount >= 32 is an edge case here too, and for some reason it's different again! (TODO: Confirm)
        if constexpr (affectFlags) 
            cpsr.carry = number >> 31;
        return ((s32) number) >> 31;
    }
}

template <bool affectFlags>
u32 inline ROR (u32 number, u32 amount) {
    auto res = Helpers::rotr (number, amount);
    if constexpr (affectFlags) {
        if (amount != 0)
            cpsr.carry = res >> 31;
    }
    return res;
}

template <bool affectFlags>
u32 inline RRX (u32 number) {
    auto res = (number >> 1) | (cpsr.carry << 31);
    if constexpr (affectFlags)
        cpsr.carry = number & 1;
    return res;
}

template <bool affectFlags>
u32 inline _ADD (u32 operand1, u32 operand2) {
    u32 res = operand1 + operand2;

    if constexpr (affectFlags) {
        setNZ (res);
        cpsr.carry = res < operand1;
        cpsr.overflow = ((operand1 ^ res) & (operand2 ^ res)) >> 31;
    }

    return res;
}

template <bool affectFlags>
u32 inline _ADC (u32 operand1, u32 operand2, u32 carryIn) {
    const u64 res = (u64) operand1 + (u64) operand2 + (u64) carryIn;

    if constexpr (affectFlags) {
        setNZ ((u32) res);
        cpsr.carry = res >> 32;
        cpsr.overflow = ((operand1 ^ (u32) res) & (operand2 ^ (u32) res)) >> 31;
    }

    return (u32) res;
}

template <bool affectFlags>
u32 inline _SBC (u32 operand1, u32 operand2, u32 carryIn) {
    const u64 subtrahend = (u64) operand2 - (u64) carryIn + (u64) 1;
    const u64 res = (u64) operand1 - subtrahend;

    if constexpr (affectFlags) {
        setNZ ((u32) res);
        cpsr.carry = subtrahend <= operand1;
        cpsr.overflow = ((operand1 ^ (u32) res) & (~operand2 ^ (u32) res)) >> 31;
    }

    return (u32) res;
}

template <bool affectFlags>
u32 inline _SUB (u32 operand1, u32 operand2) {
    const u32 res = operand1 - operand2;

    if constexpr (affectFlags) {
        setNZ (res);
        cpsr.carry = operand2 <= operand1;
        cpsr.overflow = ((operand1 ^ res) & (~operand2 ^ res)) >> 31;
    }

    return res;
}

template <bool affectFlags>
u32 inline _AND (u32 operand1, u32 operand2) {
    const u32 res = operand1 & operand2;

    if constexpr (affectFlags)
        setNZ (res);

    return res;
}

template <bool affectFlags>
u32 inline _EOR (u32 operand1, u32 operand2) {
    const u32 res = operand1 ^ operand2;

    if constexpr (affectFlags)
        setNZ (res);

    return res;
}

template <bool affectFlags>
u32 inline _ORR (u32 operand1, u32 operand2) {
    const u32 res = operand1 | operand2;

    if constexpr (affectFlags)
        setNZ (res);

    return res;
}

template <bool affectFlags>
u32 inline _BIC (u32 operand1, u32 operand2) {
    const u32 res = operand1 & ~operand2;

    if constexpr (affectFlags)
        setNZ (res);

    return res;
}

// Misaligned loads (ARM7TDMI, identical in ARM and Thumb state). The bus is never asked for a
// misaligned access; the CPU reads the aligned unit and arranges the bytes itself:
//   LDR   reads the aligned word and rotates it right by 8 bits per byte of misalignment.
//   LDRH  at an odd address reads the aligned halfword and rotates it right by 8 in 32 bits,
//         so the byte at the address lands in bits 0-7 and the byte below it in bits 24-31.
//   LDRSH at an odd address is a sign-extended byte load (LDRSB) from that address.
u32 inline loadWordRotated (u32 addr) {
    return ROR <false> (read32 (addr & ~3), (addr & 3) * 8);
}

u32 inline loadHalfRotated (u32 addr) {
    return ROR <false> ((u32) read16 (addr & ~1), (addr & 1) * 8);
}

u32 inline loadHalfSigned (u32 addr) {
    if (addr & 1) return (u32)(s32)(s8) read8 (addr);
    return (u32)(s32)(s16) read16 (addr);
}
