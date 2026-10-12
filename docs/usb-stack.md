# MMI 3G+ USB host stack — what `io-usb` and friends actually do

How the unit's USB stack behaves, read out of the binaries so a client app
(like `usb_tx_test`) can be written against it without guessing. Everything
here was verified in Ghidra on the rev `51` (E1) firmware; nothing below is
speculation unless marked **[unverified]**. The point is reuse: build on it
instead of re-reading the disassembly.

Sources (all rev `51`, under
`dev/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/mnt/ifs-root/`):

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
through `libusbdi.so.2` (and that `usb_tx_test` is built around):

1. **Do the work on your own thread, not in the callback.** io-usb's
   callbacks fire on the single `usbdi_event_handler` thread and only queue
   events; a manager thread should drain the queue. Blocking in a callback
   stalls insertion/removal/completion for the whole process.
2. **Expect to reopen on the same device.** The media launcher probes every
   new device and can detach yours (or reset it) right after it appears —
   e.g. one "link interrupted → reconnecting" is normal the moment an Android
   phone flips into accessory mode. Reopen on the same device a few times
   (usb_tx_test tries 5) before giving up.
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

`usb_tx_test` connects to io-usb with a wildcard filter and does the
[AOA](https://source.android.com/docs/core/interaction/accessories/aoa)
handshake (vendor requests 51/52/53) with any new device. An Android phone
re-enumerates as `18d1:2d00` / `2d01` with a single bulk IN/OUT interface
(`ff/ff`) — no root, no USB debugging required. Android starts the sender app
because its `accessory_filter.xml` matches manufacturer `rivit` / model
`usb_tx_test`.

**First car run (12.10, Redmi Note 8 Pro in MTP mode, `2717:ff40`, iface
06/01/01):** `usbd_connect` ok, insertion arrived, `GET_PROTOCOL` answered
**version 2**, SEND_STRING 0 and 1 went through, then SEND_STRING 2 failed with
status `0x02000005` = `CMP_ERR | DEV_NOANSWER` (a transaction the phone did
not answer at all, not a STALL), ~200 ms after the phone appeared. The app
then logged "not an Android (AOA) device" and never tried again, so START was
never sent and the phone never switched. Cause of the no-answer unknown; the
media launcher was probing the same device at the same moment (its 0xEE
probe resets a device only after a failure that is not a STALL, so it is a
suspect, not proven). Since then the handshake starts 1 s after insertion,
and any failure other than a refused `GET_PROTOCOL` is retried up to 5 times.

**Second car run (12.10 v2, same phone, same mode):** the 1 s delay and the
retries did not help. Insertion at 28.0 s. Handshake 1/5 at 29.2 s:
`GET_PROTOCOL` ok (version 2), then the **very first** `SEND_STRING` (index
0) got `DEV_NOANSWER` within 2 ms. Retries 2/5 and 3/5 (1 s and 2 s later)
got `DEV_NOANSWER` on `GET_PROTOCOL` itself, again within 0–2 ms. At 32.8 s
io-usb reported the device **removed**, and no re-insertion appeared before
the log ends (the card was pulled). So after the first failure the phone
answered nothing at all, as if it had left the bus several seconds before
io-usb noticed. Compared with v1: the failure is not tied to one string
index (v1 failed on string 2, v2 on string 0) or to time since insertion
(260 ms in v1, 1.3 s in v2). It always follows a successful `GET_PROTOCOL`
within 45–210 ms.

Open questions, in order of how cheaply they can be answered:

1. **Does this phone finish the handshake with a PC?** Run `make aoa-host`
   with the phone in the same mode (MTP, `2717:ff40`). If it fails there too,
   it is the phone (MIUI's USB mode handling); try its "charging only" mode
   and `adb logcat | grep -i -e usb -e accessory`. If it passes, it is the
   unit.
2. **What did io-usb and the media launcher do at that moment?**
   usb_tx_test now saves `sloginfo` to `var/usb_slog_<n>.txt` on the first
   handshake failure and on every removal (up to 6). run.sh cannot do it,
   because this test ends by pulling the card.

**[unverified on the car]** The full USB path end to end has host tests only
(`make aoa-host`) as of 2026-10-11. In `usb_tx_test.log`, expect the media
launcher's probe to show up as one "USB link interrupted → reconnecting" just
after the phone becomes an accessory. If the phone's MTP mode interferes
before the switch, set its USB mode to "charging only" before plugging in.
When checking a real run, confirm: the `usbd_connect` return code, which
insertion instances arrive for `18d1:2d0x` (`ff`/`42` adb instances are
skipped), the AOA `GET_PROTOCOL` result, and how many reopens happen.
