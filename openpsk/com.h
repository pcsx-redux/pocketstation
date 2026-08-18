/*
 * OpenPSK - card-link (COM) command engine interface.
 */
#pragma once

/* COM FIQ service body - runs one whole card command. Called from fiq.S on INT bit 6. */
void openpsk_com_service(void);

/* Enter card-link communication mode (init COM hardware, unmask COM FIQ, set ComFlags.9). The caller
 * must also enable FIQs at the CPU (psk_enable_fiq) for the engine to be reached. */
void openpsk_comm_enable(void);

/* Leave card-link communication mode (disable COM FIQ, reset COM hardware, clear ComFlags.9). */
void openpsk_comm_disable(void);

/* Docking sense - maintain the ComFlags that follow from an insertion/removal edge and bring the
 * card link up or down. Called from irq.S when the dock latch (INT bit 11) is pending, and once at
 * boot to pick up a device that is already docked. */
void openpsk_dock_service(void);
