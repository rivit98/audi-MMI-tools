# HBDSI — how the unit's processes call each other's services

The MMI's native processes (MMI3GApplication, MMI3GNavigation, …) are built
on Harman's MOCCA component framework. A component calls a service
interface of a component in another process, for example MMI3GNavigation
asking audio management for the nav voice channel, over **HBDSI**: a
service registry (`/usr/bin/servicebroker`) plus QNX message passing.
The Java HMI uses a different path: `/dev/dsi/data`, a resource manager
in MMI3GApplication that is exclusive per direction and held by the JVM.

Everything below is read from the P1001 binaries (MMI3GApplication,
MMI3GNavigation and servicebroker, rev `51`). The test that checks it on the
car is hello5 step `d` ([src/hello5/README.md](../src/hello5/README.md)).
Our client is [`src/common/hbdsi.c`](../src/common/hbdsi.c). The wire structs
are in [`src/common/hbdsi_wire.h`](../src/common/hbdsi_wire.h) with
`_Static_assert`s, host-tested by `tests/test_hbdsi.c`.
**[confirmed in code]**; on the car the broker does not know
`SGAudioManagement` (§4).

## 1. The servicebroker

`/usr/bin/servicebroker` (srv-starter #95) is a resource manager at
**`/srv/servicebroker`** (its usage text, in the binary's `QNX_usage`
section, gives the default). Servers register interfaces by name and
version, and clients attach to them, with devctls (dispatch
`FUN_08041f04`):

| dcmd | name | in → out |
|---|---|---|
| `0xC1180500` | REGISTER_INTERFACE | 0x118 bytes: header, version, name, then the server's chid and pid |
| `0xC00C0501` | UNREGISTER_INTERFACE | server id |
| `0xC1080502` | **ATTACH_INTERFACE** | `{u16 0, u16 1, u16 major, u16 minor, char name[256]}` → 0x20-byte reply |
| `0xC00C0503` | DETACH_INTERFACE | client id |
| `0xC01C0504/05/06`, `0xC0080507` | notifications | server disconnect / available, client detach, clear |

The devctl's `dev_info` is the broker's status: 0 ok, 1 version mismatch,
2 unknown interface, 3 already registered, 4/5/6 invalid server / client /
notification id (MMI3GApplication's log strings at `FUN_08093a10`). The
broker refuses a message header other than `(0, 0)` or `(0, 1)`.

The attach reply (`0x8042950`–`0x804297e`):

| offset | field |
|---|---|
| `+0x00` | a server field (record `+0x20`) |
| `+0x04` | the server's node, as seen from the client (`netmgr_remote_nd`) |
| `+0x08` | server pid |
| `+0x0C` | server chid |
| `+0x10`, `+0x14` | server id (record `+0x04`, `+0x08`) |
| `+0x18`, `+0x1C` | client id: a running client number, and the server id's second word |

The broker logs these as `NID: %d, PID: %X, CHID: %d, CLIENT_ID: <%d.%d>`.
Closing the fd that attached detaches the client: the broker's close handler
closes that OCB's client ids. That fires the server's client-detach
notification.

`read()` of `/srv/servicebroker` returns a status text (`FUN_0804314c`): the
broker's build, then `Connected Servers:` with one line per interface
(`" %-8d %-11s  %s  %d.%d"`: id, location, name, version), then the
notification lists.

## 2. Messages

After the attach the client sends QNX messages (`MsgSend`) to the server's
channel. Every message starts with a 0x28-byte header (the output stream's
constructor `FUN_08094328`, the connect sender `FUN_0806a364`):

| offset | field |
|---|---|
| `+0x00` | `0x200` (the first QNX message type above `_IO_MAX`) |
| `+0x04` | u16 0, u16 1 |
| `+0x08` | attach reply `+0x10`, `+0x14` (server id) |
| `+0x10` | attach reply `+0x18`, `+0x1C` (client id) |
| `+0x18` | command |
| `+0x1C` | payload bytes of the whole message |
| `+0x20` | payload bytes in this message (set at flush, `FUN_080943e4`) |
| `+0x24` | 0 |

The server's receive loop (`FUN_08066648`, `CHBDSIRootReceiverThread`) takes
these commands at `+0x18`:

| cmd | name | server's handling |
|---|---|---|
| 6 | Quit | |
| 7 | **DataRequest** | `MsgReply` empty at once, then deserialize and post the event (`FUN_08064dc0`) |
| 8 | DataResponse | same, as a response |
| 9 | **ConnectRequest** | `FUN_080659bc`, reply below |
| 10 | DisconnectRequest | |
| 15 | InternalUse | export tables between parent and child processes |

**ConnectRequest** payload (16 bytes, `+0x28`): the client's two receive
channels, its pid and its node as seen from the server. The server
`ConnectAttach`es to both channels and replies 16 bytes in the same layout:
its own channels (the first is the receive channel the request came in
on), pid and node (`FUN_08068dc4`). The client attaches to those
(`FUN_0806a1f4`). Responses and notifications come back as messages on the
client's channels, so a client must receive and reply there.

**DataRequest** payload: one serialized request event (sender
`FUN_08068540`). First the event's 16-byte base (`FUN_0806efbc`), then the
interface's arguments for that request:

| offset (message) | field |
|---|---|
| `+0x28` | 0 |
| `+0x2C` | event kind `0x100` = request (the receiver checks it here) |
| `+0x30` | request id: the interface's method number |
| `+0x34` | 0 |
| `+0x38` … | arguments, raw little-endian, in declaration order |

The server fills the target component from the connection, not from the
message.

## 3. SGAudioManagement

MMI3GApplication's AudioManagementDevCtrl serves **`SGAudioManagement`**
(stub `CSGAudioManagementStub`, request dispatch `FUN_081121b0`; implementation
vtable `0x88448dc`, each method logging `--> SGAudioManagement: …`). Its
request ids, from the dispatch and the implementation's log strings:

| id | method | arguments |
|---|---|---|
| 0 | requestRegisterSourceMapping | |
| 1 | requestRegisterSinkMapping | |
| **2** | **RequestAudioFocus** | `int32 source, int32 sink` |
| **3** | **ReleaseAudioFocus** | `int32 source, int32 sink` |
| 4 | MakeAudioSourceAudible | `int32 source, int32 sink` |
| 0x17 | GetForegroundSource | cluster |
| 0x18 | RegisterClientInterface | client name, source, sink |
| 0x19 | UnregisterClientInterface | |
| 5–0x16, 0x1A–0x1E | volume, treble, bass, balance, fader, surround, factory settings | |

The arguments of id 2 are two `int32`s (deserializer `FUN_0810fd90`).
`RequestAudioFocus` maps (source, sink) to a logical source through the
table at `0x8a353ec` (239 × `{logical source, source, sink}`,
`FUN_080ff224`) and asks the connection manager for it. **`eSRC_NAVTTS` = 55
with `eSNK_SPEAKERS` = 0 is logical source `0xE`**, one of the sources for
which `CAudioMixer::calculateTargetVolume` lowers the entertainment volume
([platform-notes.md](platform-notes.md) §9). `eSRC_SYSANN_BEEP` = 76 → `0x74`,
`eSRC_SDSTTS` = 58 → `0xD`, `eSRC_APS` = 59 → `0x7`. Source names:
`FUN_080fe778` (a jump table, source + 3). Sink names: `FUN_080ff140`.

MMI3GNavigation's own client (`FUN_08222008`, "Calling
requestRequestAudioFocus") sends id 2 with source 48 or 49 and sink 0.
Neither has a name in MMI3GApplication's source table, nor an entry in its
logical-source table, so MMI3GApplication would refuse them ("invalid
src/sink"). Either that client talks to another `SGAudioManagement` server
or its source enum differs **[unverified]**.

## 4. SGAudioManagement is not in the broker

hello5 step `d`'s attach to `SGAudioManagement` 1.0 and 1.1 gets broker
status 2, unknown interface **[confirmed]**. The broker's list has 34
interfaces. The only one from MMI3GApplication is
`PHydraGoogleServer.SPHydraGoogle` 5.0; the rest come from the phone /
Bluetooth process (`Bluephone.SGPhone`, `MediaPlayer.SGMedia`, …) and the
network process (`NWS*`, `UPNP`) **[confirmed]**. So the broker path of
§1–2 cannot reach audio management.

The parent/child path, from the code **[seen in code, not traced on the
car]** (MMI3GNavigation addresses unless noted):

- MMI3GNavigation is MMI3GApplication's child: MMI3GApplication
  `FUN_08063784` ("spawning child process %s") spawns it through
  `FUN_08080c38` with an fd map. Its command line is `MMI3GNavigation 3
  --memtrace=0 --bp --tp=/etc/DefaultScope.hbtc` **[confirmed]**; `3` is
  the child's slot in a per-child table **[inferred]** (`FUN_08080188`
  takes it). Entry `+0x08` of that table is the parent's receive chid for
  this child.
- The child signals "ready" (`FUN_08080848`): `ConnectAttach(0,
  parent_pid, entry+0x08, _NTO_SIDE_CHANNEL, 1)`, `MsgSend` of 8 bytes
  `{u32 0xB, u32 getpid()}`, then `ConnectDetach`. The parent
  (MMI3GApplication `FUN_080862e8`) receives it on that chid and takes
  word 1 (the child's pid) when word 0 is `0xB`. On failure the child logs
  "cannot signal process ready to parent process".
- The parent then sends the child an export table with the InternalUse
  command (15). The child (`FUN_080654bc`, "rec aExportTable.size() %d")
  copies each entry's server id, by interface name, into its registry at
  root `+0x50` (`0x18`-byte entries: `+0x04` name, `+0x08` u16 process
  index, `+0x0C`/`+0x10` server id). These server ids are the processes'
  own; the broker never sees them.
- Proxies are bound by role name, `"<component>.<interface>"`
  (`"AudioManagementDevCtrl.SGAudioManagement"`; CHBProxyBase
  MMI3GApplication `FUN_08095a7c` / MMI3GNavigation `FUN_0808bfb8`, "service
  with rolename %s doesn't exist!"). The navigation's audio-focus client is
  built in `FUN_08221834` / `FUN_08221930` (request id 2).
- With the server id from the export table, the child uses the §2 wire
  unchanged: ConnectRequest (9) to the parent's root channel, then
  DataRequest (7). The parent's handler (MMI3GApplication `FUN_080659bc`)
  finds the server id in its own registry; when the entry's process index
  is its own it takes the "start new service" branch and connects back to
  the client's two channels.
- Interfaces a process exports to other applications are the ones it
  registers with the broker (MMI3GApplication `FUN_08064cd0` →
  `FUN_080690c0`, REGISTER_INTERFACE); imports from other applications
  are attached through it (`FUN_08069690`, "Filled IFD for imported %s").

So what a sender needs is the parent's pid and root chid, plus the
server id of `SGAudioManagement`, which exists only inside the two
processes **[inferred]**. hello5 step `p` records, from
the kernel, which channels of MMI3GApplication the navigation and the
broker are connected to and which of its threads receive on them.
