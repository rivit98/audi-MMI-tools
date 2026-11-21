# android_auto

Android Auto head unit, proof of connectivity: a stock phone with Android
Auto, plugged into the car's USB port, connects and streams its projected
screen. Nothing is shown: the H.264 video is counted and dropped, and the
MMI shows a status page. The path that does show Android Auto is the phone
relay ([docs/android-auto.md §4.3](../../docs/android-auto.md)).

- **Run:** `android_auto <sd_path> [audio=0|1] [fps=N]` — runs until the SD
  card is pulled or SIGTERM/INT/HUP. `fps=N` holds video acks to N per
  second (default 25; 0 = ack at once).
- **Output:** `var/android_auto.log` (every protocol step; `stats:`, `pace:`, `cpu:` every 10 s), USB slog snapshots.
- **Build:**

  ```sh
  make sd SD_APPS=android_auto   # app + aa/headunit.crt, .key on the card
  make test                      # session against a fake phone + protobuf check
  make aa-host                   # desk test: the PC is the head unit (saves + plays the video)
  ```

  The first build fetches mbedTLS 3.6.7 and aasdk's head unit certificate
  into `third_party/` (pinned SHA-256s).

## How it works

- **USB:** the AOA host from audi_stream_receiver ([common/aoa_host.cpp](../common/aoa_host.cpp)),
  announcing itself as `Android` / `Android Auto`, so the phone starts Android Auto itself.
- **Frames** ([frame.h](frame.h)) and **session** ([session.cpp](session.cpp)):
  version 1.1 → TLS 1.2 → service discovery → channels (input, sensors,
  800×480 H.264 video, audio sinks) → video focus → acks and pings.
  Protobufs written by hand ([common/pb.h](../common/pb.h)) after aasdk's `.proto` files.
- **TLS** ([tls.cpp](tls.cpp)): mbedTLS client with aasdk's certificate.
  CTR-DRBG is seeded straight from `/dev/urandom` + cycle-counter jitter
  (the unit has no `/dev/random`; `mbedtls_entropy_func` fails with
  `-0x003c`). [utctime-offset.patch](../common/mbedtls/utctime-offset.patch)
  accepts the certificate's `-0700` dates.

## Results

**Works**
- Connects on the car (Redmi Note 8 Pro): all 7 channels open, TLS in ~370 ms,
  800×480 Baseline L3.1 at ~30 fps (plays on a PC).
- CPU: app 15–26 %.

**Doesn't work / open**
- The video can't be shown: the DM642 shows nothing from an 800-wide coded picture.
- Ack pacing at 25 fps makes the phone queue frames (4–7 fps, growing lag): use `fps=0`.
- The first ~20 s are slow (phone side).
- Maps needs the phone's own GPS (no location sensor offered).
- Touch / knob input and audio output not done.

Details: [docs/android-auto.md](../../docs/android-auto.md).
