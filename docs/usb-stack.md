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
because its `accessory_filter.xml` matches manufacturer `mmi3g` / model
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

**What the media launcher does at insertion (read 19.10, P1001
`vdev-medialaunch`, Ghidra project `ctf` `/mmi_usb`).** Its insertion
callback `vml_insertion` (`0x0804e960`) runs when ours does and, for a
still-image (06/xx) interface such as an Android phone in MTP/PTP mode:

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

Both 12.10 handshakes ran inside that window (+0.2 s and +1.3 s after
insertion). The 13.10 slog shows the same window for the run whose log was
lost: insertion callback 00:05:20, `Process bus event` 00:05:22, no `MTP/PFS
device detected!` (the probe failed or stalled), removal 00:05:26. A probe
failure means `usbd_reset_device`. That fits v2's pattern: one answered
request, then nothing answers, then a late removal. Two EP0 users at once
on the phone is the likely trigger **[inferred]**. Since 19.10 usb_tx_test
waits **4 s** after insertion before its first AOA request (`HANDSHAKE_FIRST_MS`),
logs the time since insertion on every handshake, and shows "USB device
xxxx:xxxx found - handshake in 4 s" on the screen. A reset phone comes back
as a new insertion and starts over.

**19.10 run:** usb_tx_test did not run at all. The card's run.sh had `APPS=hello5
hello6 hello4`, and the run ended inside hello6. So nothing on the MMI
listened when the phone was plugged in. usb_tx_test has to be the last app
and runs until the card is pulled. Anything before it that freezes the unit
(hello4's stress scene, hello6's earlier GPU phase) also costs this test.

**19.10 v2 run (`dev/19.10.2026 - v2 (usb only)/usb_tx_test.prev.log`,
same phone, same MTP mode, 4 s delay):** insertion at 29.5 s, then:

| try | after insertion | GET_PROTOCOL (IN) | SEND_STRING 0 (OUT) |
|---|---|---|---|
| 1 | 4.1 s | `DEV_NOANSWER` after 0 ms | — |
| 2 | 5.3 s | `DEV_NOANSWER` after 0 ms | — |
| 3 | 7.7 s | `DEV_NOANSWER` after 214 ms | — |
| 4 | 11.0 s | **ok, version 2** | `DEV_NOANSWER` after 2 ms |
| 5 | 15.2 s | **ok, version 2** | `DEV_NOANSWER` after 0 ms |

usb_tx_test then gave up and showed "accessory handshake failed - replug the
phone" (photo 11:27). The phone stayed on the bus until the user unplugged
it at +22.7 s. No reset, no re-insertion. The media launcher logged its
insertion callback and, 2 s later, `Process bus event - Insertion`, then
nothing until the removal. A second usb_tx_test run in the same boot saw no
device (the card was pulled at the moment the phone went in again).

What this changes:

- **The media launcher's window doesn't explain it.** Every request here
  came after its insertion work had finished (+2 s), and requests still
  failed for 15 s. The phone was not reset either: it never left the bus.
- **Failures are spread across request types.** Over the three runs with
  this phone: GET_PROTOCOL (IN, 2 bytes) 4 ok / 5 failed, SEND_STRING (OUT,
  7–22 bytes) 2 ok / 4 failed. Almost every failure is an immediate
  `DEV_NOANSWER`: the controller got no handshake at all, which a device that
  is merely slow or refusing would not cause (it would NAK, which times out,
  or STALL).
- **The stock stack fares no better with this phone.** `MTP/PFS device
  detected!` (the media launcher's MTP probe succeeding) appears in **no**
  slog we have. Slogs that cover this phone being plugged in exist from
  three boots (12.10, 13.10, 19.10).

**[inferred]** The link to this phone loses control transactions
at random. The phone's USB controller (a MediaTek SoC in a MIUI MTP gadget
mode) or the car side (the port, its adapter cable, signal quality at high
speed) are the suspects, not usb_tx_test's request format: the same
requests succeed in between.

**21.10 run: the link works end to end** (`dev/21.10.2026 - usb/`, same
phone and port, usb_tx_test alone on the card). From insertion to the first
text:

| log time | event |
|---|---|
| 83.7 s | `2717:ff40` (MTP) inserted, attach ok |
| 87.8 s | handshake 1/5, 4.1 s after insertion: `GET_PROTOCOL` ok (v2), strings and `START` ok |
| 88.1 s | removed; back as **`2717:ff40` again** 1.1 s later (not yet accessory) |
| 93.4 s | second handshake, same result; removed |
| 94.7 s | **`18d1:2d00` accessory**, EP IN `0x81` / OUT `0x01`, 512 B each |
| 113.0 s | bulk IN fails `0x02000005` (`CMP_ERR` + `DEV_NOANSWER`) after 18 s and 0 frames; the phone drops off the bus |
| 114.0 s | back as `2717:ff40`; handshake → accessory again at 119.7 s |
| 126.2 s | **`rx HELLO`** from `usb_tx_test-android/1 Xiaomi Redmi Note 8 Pro` → CONNECTED |
| 151.7 / 157.2 / 163.6 s | `TEXT` seq 1–3 received and shown (`Test`, `Dziala`, `Jaka parowa wariacie`) |

Every control request answered this time: 3 × `GET_PROTOCOL`, every
`SEND_STRING` and 3 × `START`, no `DEV_NOANSWER` on EP0 at all. The media
launcher saw each insertion and removal (slog `usb_slog_1..4.txt`) and
never got in the way. It again never logged `MTP/PFS device detected!`.

- **The phone takes two `START`s on the first connection.** After the first
  it re-enumerates as `2717:ff40` again, and only the second makes it come
  back as `18d1:2d00`. usb_tx_test's "any `2717:ff40` → handshake" loop
  handles this without a special case.
- **The first accessory session died after 18 s with no data.** The phone app
  was most likely not open yet, or its "open with" prompt was pending
  **[inferred]**. Android dropped the accessory and the phone came back in
  MTP mode. usb_tx_test reconnected on its own, and the second session
  connected 6 s after the endpoints were opened.
- **What changed since 19.10 v2 is not known.** It was the same phone, port
  and binary. Whatever made EP0 lose transactions on 12.10–19.10 (phone
  state, cable seating) was gone in this boot. If `DEV_NOANSWER` comes back,
  open questions 1–4 below still apply.

Open questions from 19.10 v2 (the link was flaky then), in order of how
cheaply they can be answered:

0. **Does the MMI's own media menu ever show this phone** (MTP) on this
   port, and does a USB stick work there? If the stock stack can't use the
   phone either, our app is not the problem.


1. **Does this phone finish the handshake with a PC?** Run `make aoa-host`
   with the phone in the same mode (MTP, `2717:ff40`). If it fails there too,
   it is the phone (MIUI's USB mode handling); try its "charging only" mode
   and `adb logcat | grep -i -e usb -e accessory`. If it passes, it is the
   unit.
2. **A different phone or cable.** Any second Android phone (AOA is
   handled by the kernel's accessory driver, so normally in any USB mode,
   "charging only" included) and, if the car uses an adapter cable, another
   cable.
3. **What did io-usb and the media launcher do at that moment?**
   usb_tx_test now saves `sloginfo` to `var/usb_slog_<n>.txt` on the first
   handshake failure and on every removal (up to 6). run.sh cannot do it,
   because this test ends by pulling the card. On 19.10 v2 io-usb logged
   nothing about the failures, so the slog can't answer this one.
4. **Not built yet, worth doing if 0–2 point at the link:** retry each
   failed control request on its own (3 times, ~50 ms apart) instead of
   restarting the handshake from GET_PROTOCOL, and before the handshake send
   ~20 standard `GET_DESCRIPTOR(device)` requests and log how many fail. That
   gives an error rate to compare between ports, cables and phones.

**[confirmed on the car, 21.10]** The full path works end to end: AOA
switch, accessory attach, bulk link, HELLO exchange, text frames. As
predicted, the first accessory session dropping shows up as one "USB link
interrupted - reconnecting". After that the reopen succeeds. When checking
a real run, confirm the `usbd_connect` return code, which insertion
instances arrive for `18d1:2d0x` (`ff`/`42` adb instances are skipped), the
AOA `GET_PROTOCOL` result, and how many reopens happen.
