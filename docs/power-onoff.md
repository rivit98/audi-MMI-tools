# Power: how the MMI 3G+ reboots and shuts down

Who decides when the unit (P1001, board rev 51) resets or powers off, which
software asks for it, and what an app of ours could use to reboot the unit
without the key combination.

Every claim is labelled:

- **[confirmed]**: read from the P1001 rev 51 binaries (address given) or
  seen in a car run (run given).
- **[inferred]**: follows from confirmed facts, not exercised.
- **[unverified]** / **[unknown]**: an assumption, or not found yet.

Addresses are virtual addresses in `MMI3GApplication` unless another binary
is named (file offset = address − `0x08040000`). Most of the on/off code is
reached only through vtables, so Ghidra's dump
(`third_party/decomp/P1001_rev51/MMI3GApplication.c`) has no functions for
it. The addresses below come from raw disassembly (`sh4-linux-gnu-objdump -b
binary -m sh4 -EL --adjust-vma=0x8040000` in the `mmi-sdk` image).

---

## 1. Summary

```
 Java HMI (lsd.jxe)
   PowerNativeConnection
     └─ DSIPowerManagement.rebootSystem() / rebootSystemCritical(slayJ9Flag)
          │ DSI
          ▼
 MMI3GApplication: COnOffPresCtrl ─► COnOffDevCtrl ─► COnOffIPC
          │ 10-byte state frame, write(fd, buf, 10)
          ▼
 /dev/ipc/ch2  (dev-ipc, FPGA link)
          │
          ▼
 IOC (V850 I/O controller): resets the SH4, or cuts its power
```

- **No standalone tool reboots or shuts down the unit.** The image has no
  QNX `shutdown` or `reboot` binary [confirmed: `bin/`, `sbin/`, `usr/bin/`,
  `usr/sbin/` of ifs-root].
- **The IOC owns power.** The SH4 can only *ask* it, through a state frame
  on `/dev/ipc/ch2` [confirmed, §4]. The reset itself happens in the IOC
  [inferred: nothing on the SH4 side can reset the CPU, §3].
- **The only code that asks is the OnOff component of MMI3GApplication.**
  The Java HMI reaches it through `DSIPowerManagement` [confirmed, §5].
- **"MMI off" is not a shutdown.** The on/off and display keys only change
  the HMI state; the SH4 keeps running until the car's bus sleeps [§2].

## 2. What the keys do

| Action | What happens |
|---|---|
| On/off key short or long press, display key | The IOC reports `SYSTEM_STATE_ONOFF_KEY_SP_REQ` / `..._LP_REQ` / `SYSTEM_STATE_DISPLAY_KEY_REQ` (strings at `0x088850b8`–`0x088851e0`) [confirmed]. The HMI goes to its off/standby state: screen dark, audio off. The SH4 keeps running [inferred]. |
| MENU + knob press + top-right soft key, held | Reboots the unit. It brought the unit back after the GPU freeze of the 2026-10-11 run (gpu-results.md §4.4) [confirmed]. Which side detects it (IOC or HMI) is [unknown]. A short or partial press only does what the single keys do. |
| Ignition off, car locked | The unit runs on for minutes and then loses power (30.10 run: ch8 byte 15 `02 → 03 → 00`, HMI `OnOff new system state is 5`, unit still running) [confirmed]. |

The run-on time is set by the timers on the GEM screen `OnOffTimer`
(`engdefs/OnOffTimer.esd`): OFF 1 / OFF 2 delay (`per 3 0x00140102/3`),
standby delay (`0x00140104`), power-down delay (`0x00140105/6`), watchdog
behaviour (`0x00140107`), BEM shutdown delay (`0x00130002`). It also has
buttons that reset the timers (`per 1 0x00050037`, `0x00050041`)
[confirmed: stock esd].

## 3. What cannot reboot the unit

- **srv-starter.** All 101 process entries in `etc/mmi3g-srv-starter.cfg`
  have `FinalAction IGNORE`, 100 have `OnTerminate IGNORE_ALL` (one
  `RESTART_ON_ERROR`), and none has a `SHUTDOWN_SYSTEM` action, although
  `srv-starter-QNX` supports one ("FINAL ACTION SHUTDOWN SYSTEM")
  [confirmed]. Killing a process never reboots the unit through srv-starter.
- **FPGA system registers.** `etc/sysregs-mmi3g-9411.cfg` has reset bits
  only for peripherals: `MicAdcRst`, `BtReset`, `GpsReset`, `NadReset`,
  `DriveReset`, `HddDrvRst`, `AudioAdcRst`, `InicRst` (register `0x0`)
  [confirmed]. None of them resets the SH4.
- **Stock GEM screens.** None has a reboot button [confirmed: the 184 esd
  files in `research/firmware-extracted/engdefs/`]. GEM `script` items run
  a shell script (`sys 1 0x0100 "/scripts/x.sh"`), but no shell command
  reboots, so a GEM button would need §6.1 or §6.2 behind it.
- **The SH4 watchdog.** The IOC watches the SH4: `EepromFlags.esd` sets
  "Watchdog SH4 status" (`per 3 0x0011000E`) to 8 minutes, 60 minutes or
  disabled [confirmed]. If MMI3GApplication stops answering, the IOC
  probably resets the SH4 after that time [unverified]. Far too slow to use.

## 4. The on/off frame on `/dev/ipc/ch2`

`COnOffDevCtrl::sendIPCData` (`0x0822cd6c`) opens `/dev/ipc/ch2` and writes
10 bytes from `this+0x134` [confirmed]. Each field has its own setter. A
setter stores the new value, marks the frame as changed (`this+0x130`), and
first sends the old frame if a change is still pending [confirmed].

| Byte | Offset | Field | Setter | Values seen [confirmed] |
|---|---|---|---|---|
| 0 | `0x134` | Interfaceversion | | |
| 1 | `0x135` | MainCPUStatus | `0x0822d818` | set by `requestSetMMIState` |
| 2 | `0x136` | DisplayStatus | `0x0822d864` | 1, or 3 then 5, during a reset request |
| 3 | `0x137` | **ShutDownRequest** | `0x0822cf88` | reset codes (below), 10, 16, 18, 22 |
| 4 | `0x138` | SuspendRequest | | |
| 5 | `0x139` | Response | `0x0822cf3c` | 1, 2 (BIOS mode result), 3 / 4 (system watchdog off at clamp S off / off entirely) |
| 6 | `0x13a` | RequestMostOnOff | `0x0822cef0` | 0, 1, 3 (restart / shut down MOST), 2 (critical unlock) |
| 7 | `0x13b` | SpecialMode | `0x0822d8b0` | 7/8 SWDL on/off, 9/10 transport, 11/10 Q-critical, 12/13, 14/15 child protection |
| 8 | `0x13c` | watchdog flag | | sent by the IOC and copied back ("received watchdog flag [%i] copy to send buffer") |
| 9 | `0x13d` | FinalShutdownFlag | `0x0822d8fc` | 2 |

### Requests in ShutDownRequest (byte 3)

| Function | Address | Byte 3 |
|---|---|---|
| `requestOff()` | `0x0822e194` | 10 ("sending shutdown request to IOPU") |
| `requestBEMShutDown()` | `0x0822e228` | 16 |
| (two further shutdown requests) | `0x0822e39c`, `0x0822ed62` | 18, 22. Overtemperature is likely one of them [unverified]. |
| `requestRestartFromOff()` | `0x0822e412` | 0 |
| `requestSendReset()` | `0x0822e08c` | the stored reset code (`this+0x11c`), then 4 if the code is 5 |

`requestReset(reason)` (`0x0822df5c`) turns the caller's reason (1–7) into
the reset code that `requestSendReset` later sends [confirmed]:

| reason | logged as | reset code |
|---|---|---|
| 1 | `ONOFF_RESET_REQ_UPDATE` | 3 |
| 2 | `ONOFF_RESET_REQ_BOLO` | 4 |
| 3 | `ONOFF_RESET_REQ_BOLO_INIC` | 5 |
| 4 | | 12 |
| 5 | | 17 |
| 6 | `ONOFF_RESET_REQ_SH4 - SWDL end` | 17 |
| 7 | | 17 |
| other | | 0 |

A plain SH4 reset is therefore most likely code 17 [inferred]. Which reason
`rebootSystem()` passes is [unknown].

Thunks enter each request function with `this − 20` (`add #-20,r4`): the
`requestReset` thunk is at `0x0822df50`, `requestSendReset` at `0x0822e080`
and `requestOff` at `0x0822e188` [confirmed].

## 5. The Java side

`lsd.jxe` (ifs-root `lsd/`) [confirmed: strings]:

- The `org.dsi.ifc.powermanagement.DSIPowerManagement` interface has
  `rebootSystem`, `rebootSystemCritical`, `setExtendedPowerState`,
  `displayButtonTyped`, `setHMIReady`, `setChildProtectionStatus`, …
- `PowerNativeConnection#rebootSystem()` logs "Power <- RebootSystem()".
  `#rebootSystem(slayJ9Flag=%1)` calls `RebootSystemCritical()`; with the
  flag set the J9 VM is first terminated (`slay`, "Termination of J9 with
  SIGNAL 11 completed!").
- Callers include the import popup ("Reboot system in %2 secs"), factory
  reset ("Rebooting system after factory reset") and the ErrorManager test
  hooks `reboot_system` / `allow_reboot_system` / `prevent_reboot_system`
  ("HMI Reset Test").

Before the reset request goes to the IOC, MMI3GApplication saves its state.
The pending persistence jobs are `ONOFF_STORE_SYSTEM_UPTIME`,
`ONOFF_STORE_FLAG_SYTEM_RESET`, `ONOFF_STORE_FSM_DATA`,
`ONOFF_SHUTDOWN_CLIENTS` and `ONOFF_BACKUP_MMEDB` [confirmed: strings near
`0x08923924`]. A "fast reset" skips this ("a fast reset is requested - no
persistent data will be stored").

### Debug command menu

`COnOffPresCtrl` has a numbered debug menu: `0` usage, `1`
setSystemStateChange, `6` rebootSystem(), `19` rebootSystemCritical, `31`/`32`
restart/shut down all on/off clients, `35`/`36` restart/shut down navi, `50`
set system time, … (strings at `0x089232f4`, used by `FUN_08416b80`). It is
dispatched by `requestTriggerDebugCommand` (`FUN_08228e98`) [confirmed]. Who
calls it is [unknown]; the Harman Becker trace tool over the network is one
candidate.

## 6. Rebooting from our side

### 6.1 Java bundle: `DSIPowerManagement.rebootSystem()` (recommended)

This is the stock path, including the persistence saves of §5.
`modules/per3-reader` already shows how an OSGi bundle obtains a DSI
service (`ServiceTracker` on the interface name). A reboot bundle would
track `org.dsi.ifc.powermanagement.DSIPowerManagement` and call
`rebootSystem()` on a trigger, such as a file on the SD card or a GEM
button [inferred, not built].

### 6.2 Native: write the frame to `/dev/ipc/ch2` (untested, riskier)

dev-ipc opens with `iofunc_open_default`, so a second process can open a
channel; our apps already read ch8 next to MMI3GApplication [confirmed]. A
native app could write a 10-byte frame with ShutDownRequest = 17 (reset) or
10 (off) [inferred]. The problem: the frame carries the whole state. We
would have to supply bytes 0–2 and 4–9, including the watchdog byte the
IOC sent last, without being able to read MMI3GApplication's current values.
A wrong MainCPUStatus or SpecialMode could put the IOC into an unexpected
mode (SWDL, transport) [unverified]. Before trying this:

1. Capture real frames. `sendIPCData` logs all fields ("COnOffDevCtrl::sendIPCData
   [%i]: Interfaceversion …") through the HB trace; if that level reaches
   the slog, a normal power cycle shows the values [unverified].
2. Find out whether reading ch2 steals the IOC's messages from
   MMI3GApplication or copies them to every reader (for ch8 both sides see
   every message) [unknown for ch2].

## 7. Open questions

- Which reason `rebootSystem()` / `rebootSystemCritical()` pass to
  `requestReset` (code 17 expected).
- Whether the reset key combination is decoded by the IOC or by
  MMI3GApplication (`SYSTEM_STATE_RESET_ON_REQ` is a candidate).
- What ShutDownRequest 18 and 22 mean.
- Whether ch8 byte 15 (`02 → 03 → 00` at ignition off) mirrors the IOC's
  terminal/shutdown state that ch2 reports to MMI3GApplication
  (`TerminalStatus`, `ShutDownRequest` in `COnOffDevCtrl::receivedIPCData`).
