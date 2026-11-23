/*
 * svcid.h - which numbers can be an HBDSI server id, and how to pick them
 * out of a block of memory. Pure: no QNX calls, host-tested
 * (tests/test_svcid.c). Step d tries the two audio ids below, in
 * svc_ids_rank's order; svc_ids_collect picks candidates out of a memory
 * dump (no longer read on the car: it found none).
 *
 * A server id is a component id from the process's bootstrap table, two
 * 16-bit halves: the low half a subsystem number, the high half the
 * component's place in that subsystem, counted from 1. MMI3GApplication's
 * subsystems run from 0x20 up (0x0001.0x0027 = AudioManagementPresCtrl,
 * 0x0002.0x0027 = AudioManagementDevCtrl), MMI3GNavigation's from 0x64
 * (0x0003.0x0064 = NavDevCtrl), so the id is unique across the unit
 * [seen in code: MMI3GApplication FUN_0804b0c0, MMI3GNavigation
 * FUN_0804c5c0's table].
 */
#ifndef HELLO5_SVCID_H
#define HELLO5_SVCID_H

#include "types.h"

/* The audio management component of MMI3GApplication, from its bootstrap
 * table: subsystem 0x27, second component. */
#define SVC_ID_AUDIO_DEVCTRL 0x00020027u
#define SVC_ID_AUDIO_PRESCTRL 0x00010027u

/* How many candidate ids we ever carry around. */
#define SVC_MAX_IDS 8

#define SVC_SUBSYS_MIN  0x20
#define SVC_SUBSYS_MAX  0xff
#define SVC_INSTANCE_MAX 0x3f

/* Could v be a component id? */
int svc_id_shape(u32 v);

/* Every 4-byte aligned word of buf that could be a component id, added to
 * ids[] (which already holds n of them, duplicates skipped, at most max).
 * The new count. Words are read in the machine's own byte order: the unit
 * and the host are both little-endian. */
unsigned svc_ids_collect(const u8 *buf, usize len, u32 *ids, unsigned max,
                         unsigned n);

/* ids[] reordered so that the ones most likely to be audio management
 * come first: the audio subsystem (0x27) before anything else, and within
 * it the known DevCtrl id first. Keeps every id. */
void svc_ids_rank(u32 *ids, unsigned n);

#endif
