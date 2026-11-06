# bridge: a laptop shell on the MMI over USB (parked)

**Status: parked 2026-11-05. Nothing is built.** This doc records the idea,
why the obvious version can't work, and which options are left, so the work
can start from here.

Labels:

- **[confirmed]**: checked on the hardware or in the binaries.
- **[inferred]**: follows from confirmed facts.
- **[unverified]**: from memory or datasheets, not checked yet.

---

## 1. The idea

Today every change means a trip with the SD card: build, copy to the card,
walk to the car, insert, wait, pull, read the logs. bridge would leave only
the first trip:

1. Insert the SD card once. `run.sh` starts `bridge` on the MMI, a
   receiver/executor that stays running.
2. Plug the laptop into the MMI's USB port.
3. From the laptop, a CLI gives:
   - a remote **interactive shell** (`ksh`), and one-off `exec <cmd>` with its
     output and exit code;
   - a quick **upload** of files to the MMI (e.g. a fresh `build/hello9` into
     `/tmp/mmi_apps/`), and possibly download (logs, screenshots).

Wanted: a Linux-native laptop tool, not an Android app.

## 2. Why a plain cable between the laptop and the MMI can't work

A USB link always has one **host**, which starts every transfer, and one
**device**, which answers. Each role needs its own controller hardware.

| Side | Controller | Can be a USB device? |
|---|---|---|
| MMI | EHCI/OHCI host, driven by `io-usb` (docs/usb-stack.md) | no **[confirmed]** |
| Android phone | dual-role (OTG) | yes. The phone was the device for AOA (`18d1:2d00`, car run 21.10) **[confirmed]** |
| Laptop (Lenovo `21CFCTO1WW`, AMD Rembrandt) | xHCI host only (USB4 XHCI `1022:161d/161e/161f/15d6/15d7`) | **no**. `/sys/class/udc/` doesn't exist, so Linux sees no device-mode controller **[confirmed 2026-11-05]** |

So the **A-to-C cable used with the phone** is fine as a cable. With the
laptop on the C end, though, both sides are hosts waiting for a device, and
nothing enumerates on either side. It is electrically safe: the A-to-C
cable's Rp pull-up makes the laptop's C port a power sink, so it doesn't drive
VBUS. No software can fix this: the silicon that answers as a device is
missing. **[inferred]**

The laptop's USB4 ports can do host-to-host networking (`thunderbolt-net`),
but only with another USB4/Thunderbolt host. The MMI's port is USB 2.0.

**Never use an A-to-A cable.** It joins the two hosts' 5 V VBUS lines. The
supplies are never exactly equal, so current flows backwards into the weaker
one. That trips the port's over-current protection at best, and burns a power
switch or polyfuse (laptop or MMI) at worst. The USB spec forbids such cables
for that reason. The only sound A-to-A product is one with a bridge chip in
the middle, which keeps the two VBUS lines apart.

## 3. Options left

Each option puts something that **can** be a USB device on the MMI's port.

### 3.1 Host-to-host bridge cable (preferred if hardware is bought)

A Prolific **PL27A1** (USB 3.0) or **PL25A1** (USB 2.0) "data link / easy
transfer" cable, about €15 (Plugable, Unitek and Belkin rebadges). Its chip
looks like an ordinary USB device to **both** hosts, and data written to one
side's bulk OUT endpoint arrives on the other side's bulk IN. **[unverified]**

- **MMI side:** `bridge` opens the chip with libusbdi. That is the same
  pattern as audi_stream_receiver's AOA client (wildcard `usbd_connect`,
  match VID/PID, bulk IN/OUT, reopen on `ABORTED`, retry close/detach on
  `EBUSY`: docs/usb-stack.md "What this means for writing a client").
- **Laptop side:** a libusb (pyusb) tool in Docker with `/dev/bus/usb`, like
  `make aoa-host` (tools/aoa_host.py). Linux's `plusb` kernel driver claims
  the chip as a network interface, so the tool has to detach it first.
- **What has to be checked:**
  - **[unverified]** IDs. From Linux's `drivers/net/usb/plusb.c`:
    `067b:25a1` (PL25A1), `067b:27a1` (PL27A1), `050d:258a` (Belkin).
  - **[unverified]** Link setup. `plusb` sends one vendor request,
    `bRequest 3` (set QuickLink feature), `wValue = S_EN|RESET_OUT|RESET_IN|PEER_E`
    (`0x99`), device-to-host, length 0. Check against the source before relying on it.
  - **[unverified]** PL27A1 cables sometimes have a hardware mode switch
    (e.g. "Windows/Mac" vs "Android"), and the wrong mode may enumerate as
    something else.
  - The MMI's media launcher probes every new device (MTP `0xEE` probe, may
    `usbd_reset_device` on failure: docs/usb-stack.md). Expect one reconnect
    when the cable is plugged in, as with the phone.
  - Speed: USB 2.0 high speed on the MMI, so roughly 20+ MB/s minus the
    MMI's CPU cost. **[inferred]**

### 3.2 The phone as the go-between (no new hardware)

```
laptop ──Wi-Fi / wireless adb──► phone app ──USB, AOA──► bridge on the MMI
```

The MMI-to-phone link already works (audi_stream_receiver, car runs
21.10-05.11). The phone app would relay a byte stream between the AOA link
and a TCP socket, and the laptop connects with `adb forward` over wireless
debugging or straight over Wi-Fi.

The cost: a second Android app (or a mode in the existing one), one more hop
that can fail, and the phone's screen-off / app-kill behaviour.

### 3.3 A small board with a device controller

A Raspberry Pi Zero (2) W, or any board with a UDC, in USB gadget mode
(`g_serial` / FunctionFS) on the MMI's port. The laptop reaches the Pi over
Wi-Fi or its second port. It works, but it's more pieces than a cable for the
same result.

### 3.4 Not USB, for comparison

- **ASIX USB-Ethernet dongle** (`modules/usb-net`) plus a LAN cable. This
  already gives telnet (port 23, and a raw shell on 2323 through inetd) with
  no new MMI code. Upload would still need something (e.g. `nc`, if the unit
  has it, or a small receiver).
- **The car's own Wi-Fi AP** (`uap0` 192.168.1.1, CLAUDE.md). No hardware is
  needed, only a TCP listener started from the SD card.

## 4. Sketch, for whenever this is picked up

These are design notes, not decisions.

- **One protocol, several transports.** Length-prefixed frames, each with a
  channel id: shell stdin/stdout, exec, file put/get, control. Send the same
  stream over the bridge cable, AOA, or TCP. A TCP transport lets `bridge` be
  built for Linux and the whole protocol tested on the desk, before any car run.
- **Shell:** `ksh -i` on a pty (QNX `openpty` / `/dev/ptyp*`, to be checked
  on 6.3.2) so line editing and Ctrl-C work. Fall back to plain pipes if the
  pty route fails.
- **Upload:** write to a temp name, check CRC32 or a length, `rename()`, keep
  the mode bits (`chmod +x`). The default target is `/tmp/mmi_apps/`, because
  `/` and the efs partitions are read-only or full.
- **Safety:** no auth beyond physical access (a USB cable in the car). Never
  start it by default. It runs only while the SD card that started it is in,
  like audi_stream_receiver.
- **Logging:** every connection, command and transfer goes to the card
  (`var/bridge.log`), as the other apps do.

## 5. Open questions

1. Which option (§3.1 cable, §3.2 phone, §3.3 board)? This decides the
   transport.
2. If §3.1: which cable, and do its IDs and its link-setup request match
   §3.1's list?
3. Does QNX 6.3.2 on the unit give us a pty (for an interactive shell), and
   is `nc` or anything similar on the unit?
