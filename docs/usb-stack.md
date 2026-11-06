# MMI 3G+ USB host stack — what `io-usb` and friends actually do

How the unit's USB stack behaves, read out of the binaries so a client app
(like `audi_stream_receiver`) can be written against it without guessing. Everything
here was verified in Ghidra on the rev `51` (E1) firmware; nothing below is
speculation unless marked **[unverified]**. The point is reuse: build on it
instead of re-reading the disassembly.

Sources (all rev `51`, under
`third_party/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/mnt/ifs-root/`):

| Binary | Role | Where on the unit |
|---|---|---|
| `io-usb` | QNX USB host stack (EHCI + OHCI), started by srv-starter | `usr/sbin/io-usb` |
| `libusbdi.so.2` | USBDI client library apps link against | `mnt/efs-system/lib`, symlinked to `/lib` |
| `vdev-medialaunch` | the media launcher: probes/attaches new USB devices | `usr/sbin/vdev-medialaunch` |
| `iofs-pfs.so` | MTP/PTP filesystem driver the media stack loads | `usr/lib/iofs-pfs.so` |

> **Ghidra loading note.** `libusbdi.so.2` and `iofs-pfs.so` are rebased at
> `0x10000`, so their addresses below are relative to that. `io-usb` from the
> efs extraction has a corrupt section-header table — load a copy with
> `e_shoff`/`e_shnum` zeroed. (Ghidra project `ctf`, folder `/mmi_usb` has all
> four already imported.)

---

## The questions that matter for a client app

| Question | Answer from the binaries | Where |
|---|---|---|
| Can another client lock us out of the phone? | **No.** `udi_attach` only counts openings; it refuses nothing but hubs. There is no attach exclusivity. | io-usb `0x08046040` |
| Does the media stack's MTP driver take the accessory? | **No.** `ptp_parse_descriptors` needs bulk IN + bulk OUT + interrupt; the accessory interface (`ff/ff`) has no interrupt endpoint, so MTP rejects it. | iofs-pfs `0x27940` |
| Do unplug / abort / detach complete our URBs? | **Yes** — all three complete them with `ABORTED`, and send the completion event **before** the removal event. | io-usb `udi_in_out 0x08046620`, `0x08044e80`, `udi_transfer_complete 0x08044d40` |
| Can the link break while the phone stays plugged? | **Yes.** A client's `usbd_detach` aborts **every** URB on the interface's endpoints — other clients' included — and the media launcher attaches/probes/detaches new devices. Its MTP probe also **resets** a device whose `GET_DESCRIPTOR(0xEE)` times out (a STALL is fine). This is why a client must be ready to reopen. | io-usb `udi_detach 0x08046400`; medialaunch `0x080539c0`, `0x080537e0` |
| Removal while our URBs are pending? | libusbdi defers our `removal()` callback until they complete; **no deadlock.** | libusbdi `usbdi_event_thread 0x17a20`, `usbdi_synchronise` |
| Close/detach races | `usbd_close_pipe` / `usbd_detach` return `EBUSY` (and free nothing) while libusbdi still counts outstanding I/O, which it decrements only **after** our callback returns. A device never detached keeps io-usb's port in "holdoff" even after an unplug. So **both calls must be retried** until the counters drain. | libusbdi `0x15aa0`, `0x15820`; io-usb `CLASS_ExtractDevice 0x08048e60` |
| Synchronous vs asynchronous `usbd_io` | Decided by the endpoint: EP0 waits for the reply (control transfers are synchronous), every other endpoint is asynchronous and completes on the event thread. | libusbdi `usbd_io 0x19360` |
| Event thread | **One** thread (`usbdi_event_handler`, 16 KB stack) runs insertion, removal and completion callbacks, in order. | libusbdi `0x17ca0` |

---

## What this means for writing a client

These are the consequences the above forces onto any app that opens a device
through `libusbdi.so.2` (and that `audi_stream_receiver` is built around):

1. **Do the work on your own thread, not in the callback.** io-usb's
   callbacks fire on the single `usbdi_event_handler` thread and only queue
   events; a manager thread should drain the queue. Blocking in a callback
   stalls insertion/removal/completion for the whole process.
2. **Expect to reopen on the same device.** The media launcher probes every
   new device and can detach yours (or reset it) right after it appears —
   e.g. one "link interrupted → reconnecting" is normal the moment an Android
   phone flips into accessory mode. Reopen on the same device a few times
   (audi_stream_receiver tries 5) before giving up.
3. **Retry `usbd_close_pipe` / `usbd_detach` on `EBUSY`.** They free nothing
   and the port stays in holdoff until libusbdi's I/O counters drain, which
   happens only after your completion callbacks have returned. Loop until they
   succeed.
4. **Don't rely on exclusivity, and don't fear it.** No one can lock you out,
   and MTP won't steal an accessory interface — but another client's detach
   can abort your URBs, so treat `ABORTED` as "reopen", not "fatal".
5. **Control transfers (EP0) are synchronous; bulk is async.** Your AOA
   handshake on EP0 returns inline; bulk IN/OUT complete later on the event
   thread.

---

## USBDI header (`sdk/include/sys/usbdi.h`)

openqnx ships no USB DDK headers, so `sdk/include/sys/usbdi.h` is
**reconstructed** from the QNX 6.5.0 SDK header (same ABI generation) and
checked against the unit's `libusbdi.so.2` disassembly. Verified against the
binary:

- `usbd_connect_parm_t` field offsets.
- Accepted `USBD_VERSION` values: `0` or `0x0101`.
- Argument order of `usbd_setup_vendor` / `usbd_setup_bulk`.
- Buffers must come from `usbd_alloc()` — the library calls `usbd_mphys()` on
  them, so a plain `malloc` buffer will not DMA.

The usage pattern follows smartdevicelink's QNX USB transport, which shipped
on QNX head units, and is the reference for the AOA client flow.

---

## Android Open Accessory, as seen by this stack

`audi_stream_receiver` connects to io-usb with a wildcard filter and does the
[AOA](https://source.android.com/docs/core/interaction/accessories/aoa)
handshake (vendor requests 51/52/53) with any new device. An Android phone
re-enumerates as `18d1:2d00` / `2d01` with a single bulk IN/OUT interface
(`ff/ff`) — no root, no USB debugging required. Android starts the sender app
because its `accessory_filter.xml` matches manufacturer `mmi3g` / model
`audi_stream_receiver`.

### What the media launcher does at insertion

From P1001 `vdev-medialaunch` (Ghidra project `ctf` `/mmi_usb`). Its
insertion callback `vml_insertion` (`0x0804e960`) runs when ours does and,
for a still-image (06/xx) interface such as an Android phone in MTP/PTP
mode:

1. runs the MTP probe `0x080539c0`: attaches, sends `GET_DESCRIPTOR(string
   0xEE)` on EP0 (send helper `0x080537e0`, `usbd_io` with the default
   timeout). On success it sends the MS OS vendor request and logs `MTP/PFS
   device detected!`. On any failure **except a STALL** it calls
   **`usbd_reset_device`**. Then it detaches.
2. logs `Selecting usb configuration` and calls `usbd_select_config`
   (SET_CONFIGURATION on EP0);
3. signals its bus event thread, which waits the `-c` delay ("between device
   configuration selection and insertion notification", **default 2000
   ms**; srv-starter passes `-z -C -f /etc/umass-enum.cfg -R -I usbpfs -n 1
   -H poll=1000,hub_detect`, so no `-c`) and then logs `Process bus event -
   Insertion` and starts the MTP driver.

So for ~2 s after insertion the media launcher may be using EP0 on the same
device, and a failed probe resets it. audi_stream_receiver waits **4 s**
after insertion before its first AOA request (`HANDSHAKE_FIRST_MS`), logs
the time since insertion on every handshake, and shows "USB device
xxxx:xxxx found - handshake in 4 s" on the screen. A reset phone comes back
as a new insertion and starts over. Any handshake failure other than a
refused `GET_PROTOCOL` is retried, up to 5 times.

### On the car (Redmi Note 8 Pro, MTP mode `2717:ff40`, iface 06/01/01)

**[confirmed]** The full path works: AOA switch, accessory attach, bulk
link, `HELLO` exchange. A working connection, in order:

1. `2717:ff40` (MTP) inserted, attach ok.
2. Handshake 4.1 s after insertion: `GET_PROTOCOL` ok (version 2), strings
   and `START` ok.
3. The phone is removed and comes back as **`2717:ff40` again** (not yet
   accessory). A second handshake, same result, removed again.
4. **`18d1:2d00` accessory**, EP IN `0x81` / OUT `0x01`, 512 B each.
5. If the phone app doesn't open the accessory (not open yet, or its "open
   with" prompt pending **[inferred]**), bulk IN fails `0x02000005`
   (`CMP_ERR` + `DEV_NOANSWER`) after ~18 s with 0 frames, and the phone
   drops back to MTP. This shows up as one "USB link interrupted -
   reconnecting"; audi_stream_receiver reconnects on its own.
6. Next handshake → accessory → `HELLO` → CONNECTED.

- **The phone takes two `START`s on the first connection.**
  audi_stream_receiver's "any `2717:ff40` → handshake" loop handles this
  without a special case.
- The media launcher sees each insertion and removal and doesn't get in
  the way. It never logged `MTP/PFS device detected!` for this phone, in
  any slog we have.
- **The app must run when the phone is plugged in.** It runs until the card
  is pulled, so it has to be the last app of a run; an app before it that
  freezes the unit costs the USB test too.

**EP0 losing transactions.** In some boots, with the same phone, port and
binary, control requests failed with status `0x02000005` = `CMP_ERR |
DEV_NOANSWER` (a transaction the phone did not answer at all, not a
STALL), and the handshake never completed. In other boots every control
request was answered. What differs between those boots is not known.

- Failures hit both request types: `GET_PROTOCOL` (IN, 2 bytes) 4 ok / 5
  failed, `SEND_STRING` (OUT, 7–22 bytes) 2 ok / 4 failed. Not tied to
  one string index or to time since insertion (0.2–15 s).
- Almost every failure is an immediate `DEV_NOANSWER` (0–2 ms, once
  214 ms): the controller got no handshake at all, which a device that is
  merely slow or refusing would not cause (it would NAK, which times out,
  or STALL). A failing `SEND_STRING` came 45–210 ms after a successful
  `GET_PROTOCOL`.
- Once the phone answered nothing after the first failure and io-usb
  reported it removed ~4 s later; another time it stayed on the bus,
  never reset, until unplugged.
- **The media launcher's window doesn't explain it:** requests sent after
  its insertion work had finished (+2 s) still failed for 15 s. Starting
  the handshake later (1 s, then 4 s) and retrying it from
  `GET_PROTOCOL` did not help.
- io-usb logs nothing about these failures in the slog.

**[inferred]** The link to this phone loses control transactions at
random. The phone's USB controller (a MediaTek SoC in a MIUI MTP gadget
mode) or the car side (the port, its adapter cable, signal quality at high
speed) are the suspects, not audi_stream_receiver's request format: the
same requests succeed in between.

### Open questions (if `DEV_NOANSWER` comes back)

In order of how cheaply they can be answered:

1. **Does the MMI's own media menu ever show this phone** (MTP) on this
   port, and does a USB stick work there? If the stock stack can't use the
   phone either, our app is not the problem.
2. **Does this phone finish the handshake with a PC?** Run `make aoa-host`
   with the phone in the same mode (MTP, `2717:ff40`). If it fails there too,
   it is the phone (MIUI's USB mode handling); try its "charging only" mode
   and `adb logcat | grep -i -e usb -e accessory`. If it passes, it is the
   unit.
3. **A different phone or cable.** Any second Android phone (AOA is
   handled by the kernel's accessory driver, so normally in any USB mode,
   "charging only" included) and, if the car uses an adapter cable, another
   cable.
4. **Not built, worth doing if 1–3 point at the link:** retry each
   failed control request on its own (3 times, ~50 ms apart) instead of
   restarting the handshake from `GET_PROTOCOL`, and before the handshake
   send ~20 standard `GET_DESCRIPTOR(device)` requests and log how many
   fail. That gives an error rate to compare between ports, cables and
   phones.

When checking a log, confirm the `usbd_connect` return code, which
insertion instances arrive for `18d1:2d0x` (`ff`/`42` adb instances are
skipped), the AOA `GET_PROTOCOL` result, and how many reopens happen. On a
handshake failure and on every removal (up to 6) audi_stream_receiver saves
`sloginfo` to `var/usb_slog_<n>.txt`.
