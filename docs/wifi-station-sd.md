# Wi-Fi station from the SD card: shell and file transfer over a phone hotspot

Can the head unit (P1001, board rev 51) join a phone hotspot and give the
laptop a shell and a way to upload binaries, with everything run from the
SD card and nothing installed on the unit? This covers what the toolkit's
`wifi-sta` module already provides, what P1001 has for remote access, and
a card-only design built from both.

Every claim is labelled:

- **[confirmed]**: read from the P1001 rev 51 images or the module's
  scripts (path given).
- **[inferred]**: follows from confirmed facts, not exercised.
- **[unverified]**: an assumption, not checked yet.

Paths: the IFS tree is
`third_party/HN+R_EU_AU_P1001/extracted/MU9411/ifs-root/51/mnt/ifs-root/`,
the efs-system tree is
`third_party/decomp/P1001_rev51/work/config_survey/efs/mnt/efs-system/`,
the module is `../../modules/wifi-sta/` (repo root `modules/wifi-sta/`).

**Status:** design only, nothing built or run on the car.

---

## 1. Summary

```
 phone hotspot (2.4 GHz, WPA2-PSK)
      ▲  en6, DHCP lease
      │
 88W8688 in station mode: devn-mv8688sta.so + sd8688.bin + wpa_supplicant
      │  all copied from the card to /fs/tmpfs, started by copie_scr.sh
      ▼
 inetd (factory) ── 2323: root ksh, no password
                 ── 2324: upload   (cat > file)
                 ── 2325: download (cat file)
```

- The station switch in `wifi-sta` is already RAM-only; only the module's
  installer writes to the unit. Calling `sta-mode.sh` straight from the
  card gives a station with no installation.
- SSH is not needed: P1001 has `inetd`, `telnetd` and `login`, and
  `inetd` serves any command on any port.
- The one file the module needs on the unit is the LAN-mode flag; it can
  be dropped if only the shell is wanted (§6).

---

## 2. What `wifi-sta` provides

**[confirmed]** The factory driver `devnp-mv8688uap.so` is Marvell's
access-point (uAP) driver and cannot run station mode; the module ships a
new station driver, `wpa_supplicant` 2.11, Marvell's station firmware and
`jitter-egd` (the unit has no `/dev/urandom`; `wpa_supplicant` reads its
random bytes from `jitter-egd` on `127.0.0.1:708`). See
`research/WIFI_STATION_MODE.md`.

**[confirmed]** `payload/sta-mode.sh start [-r dir] [-c config]` keeps all
its state in `/fs/tmpfs/mmi-sta`, takes the station files from `dir` and
the hotspot from `config`, and starts nothing at boot. A restart brings
back the factory access point. Station and access point cannot run
together: the chip holds one firmware at a time.

**[confirmed]** The installer (`scripts/wifi_sta_run.sh`) and
`sta-mode.sh` refuse unless three factory files match the unit the module
was validated on (9411, K0942, MU 1038). P1001 rev 51 matches all three:

| File | Expected `cksum` | P1001 rev 51 |
|---|---|---|
| `/lib/dll/devnp-mv8688uap.so` | `1022510863 93538` | same (IFS) |
| `/usr/sbin/dhcp.client` | `263280565 57048` | same (IFS) |
| `/etc/dhcp/dhcp-up` | `3046797907 6292` | same (efs-system `pss/nws/usr/bin/dhcp-up`) |

**[confirmed]** `sta-mode.sh` also refuses when `/mnt/efs-system/RSE`
exists; the stock P1001 efs-system has no such file.

**[confirmed]** What the installer adds and a card-only run does not need:
the HDD copy (`/mnt/persistence/wifi-sta`), the start-up block in
`/mnt/efs-system/lsd/lsd.sh`, and the `WifiClient.esd` GEM screen.

---

## 3. Choosing the hotspot

**[confirmed]** The unit does not scan or offer a choice: it joins the one
network in a `wpa_supplicant` configuration file. `sta-mode.sh` requires
exactly one network with `ssid=` and `key_mgmt=WPA-PSK`, owned by root,
mode 600.

```
network={
    ssid="My Phone"
    psk=<64 hex digits from: wpa_passphrase "My Phone" "passphrase">
    key_mgmt=WPA-PSK
}
```

Using the hex `psk` keeps the passphrase off the card. The card script
copies the file to `/fs/tmpfs` and sets mode 600 there (FAT has no
modes).

**[confirmed]** The 88W8688 is 2.4 GHz only and the supplicant runs
WPA2-PSK/CCMP: the hotspot must offer 2.4 GHz and WPA2 (iPhone: Maximise
Compatibility; Android: 2.4 GHz band, WPA2 or WPA2/WPA3).

---

## 4. Shell: telnet, not SSH

**[confirmed]** The SSH that `wifi-sta` serves on port 2222 is
[chundla/qnx632-openssh](https://github.com/chundla/qnx632-openssh)'s.
It is not packaged in the toolkit, has no released binaries (needs the
QNX 6.3.2 SDK or our toolchain), and its installer is locked to the test
unit's `lsd.sh` (`2252741285 11676`; stock P1001: `3702683410 3682`).

**[confirmed]** P1001 IFS has `/usr/sbin/inetd`, `/usr/sbin/telnetd`,
`/bin/login`, and `/etc/inetd.conf` enables `telnet ... in.telnetd`. The
toolkit's `usb-net` module already starts them
(`modules/usb-net/scripts/usb_net_run.sh`).

**[confirmed]** The stock `scripts/Connectivity/pf.conf` blocks inbound
only on `uap0`; `en6` is not named and the default is pass.
**[inferred]** Telnet on the station address is reachable without a pf
rule.

- Port 23: `login`, needs the unit's root password.
- Port 2323: `ksh -i` as root, no password.

No authentication or encryption: anyone on the hotspot gets root. On a
phone hotspot that is only the owner's devices.

---

## 5. File transfer

**[confirmed]** The unit has no `nc`, `base64`, `uudecode`, `tar`, `dd`,
`od` or `ftpd`; it has `cksum`, `sed`, `chmod`, `fs-nfs3` (efs-system
`usr/sbin`), `fs-cifs` (IFS `usr/sbin`) and `qconn` (efs-system `bin`).

### 5.1 Raw TCP through inetd (preferred)

`inetd` hands the accepted socket to the command as stdin/stdout, so one
`cat` per port is an upload or download channel.

On the unit:

```sh
mkdir -p /fs/tmpfs/up
echo 'cat > /fs/tmpfs/up/incoming' > /fs/tmpfs/recv.sh
echo 'cat /fs/tmpfs/up/outgoing'   > /fs/tmpfs/send.sh
cat > /dev/shmem/inetd_dev.conf <<EOF
2323 stream tcp nowait root /bin/ksh ksh -i
2324 stream tcp nowait root /bin/ksh ksh /fs/tmpfs/recv.sh
2325 stream tcp nowait root /bin/ksh ksh /fs/tmpfs/send.sh
EOF
/usr/sbin/inetd /dev/shmem/inetd_dev.conf &
```

On the laptop:

```sh
nc -N <mmi-ip> 2324 < mybin     # upload (traditional nc: -q1)
cksum mybin                     # compare with the unit
nc <mmi-ip> 2325 > out.bin      # download /fs/tmpfs/up/outgoing
```

Then over port 2323:

```sh
mv /fs/tmpfs/up/incoming /fs/tmpfs/up/mybin
chmod +x /fs/tmpfs/up/mybin
cksum /fs/tmpfs/up/mybin
```

One file per upload under a fixed name; `cksum` checks every transfer.
`/fs/tmpfs` is RAM and is cleared by a restart.

### 5.2 Mounting a laptop directory [unverified]

```sh
fs-nfs3 <laptop-ip>:/srv/mmi /mnt/lap
fs-cifs //<laptop>:<laptop-ip>:/share /mnt/lap <user> <pass>
```

Binaries then run straight from the laptop's build directory. Neither has
been tried on the unit. The QNX 6.3 NFS client may want UDP, which recent
Linux NFS servers disable by default; `fs-cifs` likely needs SMB1 on the
server (`server min protocol = NT1`).

### 5.3 The card

Binaries can still ride on the card next to `copie_scr.sh`, as the apps
in this tree do.

---

## 6. Card-only design

On card insert, `copie_scr.sh`:

1. copies the `wifi-sta` payload (driver, firmware, `wpa_supplicant`,
   `jitter-egd`, `sta-mode.sh`) and `hotspot.conf` to
   `/fs/tmpfs/wifi-sta`, sets the configuration to mode 600;
2. runs `sta-mode.sh start -r /fs/tmpfs/wifi-sta -c /fs/tmpfs/wifi-sta/hotspot.conf`;
3. starts `inetd` with the §5.1 configuration;
4. writes the lease address and `sta-mode.sh status` to a log on the
   card.

Running from RAM lets the card come out afterwards. A restart returns the
stock unit with its access point.

**The LAN-mode flag.** `sta-mode.sh` refuses without
`/HBpersistence/DLinkReplacesPPP` (an empty file; takes effect after a
restart). It is what makes the factory `dhcp-up` give the HMI the lease's
default route and DNS, i.e. the MMI's online services over the hotspot.
For a shell alone it is not needed; removing that check from the card's
copy of `sta-mode.sh` leaves the unit with no writes at all.
**[inferred]** Without the flag the station still gets its lease and
address; the HMI just does not route through it.

**Open points [unverified]:**

- whether `proc_scriptlauncher` runs `copie_scr.sh` for a card already in
  the slot at boot, or only on insert;
- that `jitter-egd` alone (no OpenSSH installation) works on the unit:
  the module ships it, but it has never run without OpenSSH;
- a way to stop the station without a restart (a second card or a flag
  file running `sta-mode.sh stop`).

---

## 7. Risks

- Station mode removes the car's access point until a restart; anything
  using `uap0` (phones on the car's Wi-Fi) loses it. USB AOA streaming is
  unaffected.
- Root shell with no password on 2323, plaintext.
- The card holds the hotspot `psk`.
- Broadcast frames drop (up to about 40 %) while Bluetooth audio streams;
  unicast is unaffected (from the module's README).
