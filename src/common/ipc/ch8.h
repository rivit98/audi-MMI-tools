/*
 * ch8.h - the layout of a /dev/ipc/ch8 message: the state of every knob and
 * button at once (buttons/README.md has the protocol and how it was found).
 * C, so C apps and quit.c can name a button; buttons/ decodes the messages
 * into events for C++ apps.
 *
 *   bytes 0-13  controls: button bits (set while held), except
 *   bytes 4-7   knob detents since the last message (5/4 volume, 7/6 main)
 *   byte 14     01 = controls, 02 = touch pad (control bytes all zero)
 *   byte 15     02 while running (the motorised screen)
 */
#ifndef COMMON_IPC_CH8_H
#define COMMON_IPC_CH8_H

#include "types.h"

#define CH8_MSG_BYTES      27   /* a plain message; touch pad ones are 29-33 */
#define CH8_CONTROL_BYTES  14
#define CH8_KIND_BYTE      14
#define CH8_KIND_TOUCH     0x02
#define CH8_POWER_BYTE     15

/* A button bit as one int: byte << 8 | mask. 0 = none. */
#define CH8_KEY(byte, mask)  ((byte) << 8 | (mask))
#define CH8_KEY_BYTE(k)      ((k) >> 8)
#define CH8_KEY_MASK(k)      ((k) & 0xff)

#define CH8_RADIO              CH8_KEY(0, 0x01)
#define CH8_MEDIA              CH8_KEY(0, 0x02)
#define CH8_TEL                CH8_KEY(0, 0x08)
#define CH8_NAV                CH8_KEY(0, 0x10)
#define CH8_CAR                CH8_KEY(0, 0x40)
#define CH8_NEXT               CH8_KEY(1, 0x01)
#define CH8_SOFT_TOP_LEFT      CH8_KEY(1, 0x02)
#define CH8_SOFT_BOTTOM_LEFT   CH8_KEY(1, 0x04)
#define CH8_PREV               CH8_KEY(1, 0x08)
#define CH8_SOFT_TOP_RIGHT     CH8_KEY(1, 0x10)
#define CH8_SOFT_BOTTOM_RIGHT  CH8_KEY(1, 0x20)
#define CH8_BACK               CH8_KEY(1, 0x40)
#define CH8_MAIN_KNOB_PRESS    CH8_KEY(1, 0x80)
#define CH8_VOLUME_KNOB_PRESS  CH8_KEY(2, 0x01)
#define CH8_MENU               CH8_KEY(3, 0x04)

/* 1 if msg[0..n) is a controls message with button key held. */
static inline int ch8_held(const u8 *msg, int n, int key)
{
	return key != 0 && n > CH8_POWER_BYTE && msg[CH8_KIND_BYTE] != CH8_KIND_TOUCH &&
	       (msg[CH8_KEY_BYTE(key)] & CH8_KEY_MASK(key)) != 0;
}

#endif
