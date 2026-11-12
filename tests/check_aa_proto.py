"""check_aa_proto.py prepare|check <dir> <proto_root>  (make test, mmi-aa-proto image).

Checks android_auto's hand-written protobuf (src/common/pb.h, the messages
in src/android_auto/session.cpp) against Google's protobuf and aasdk's
.proto files (<proto_root>/aap_protobuf/..., fetched by make):

prepare: compiles every .proto with protoc into <dir>/py, and writes the
         messages the phone sends, encoded by protobuf, to
         <dir>/phone/<name>.bin. build/test_android_auto's fake phone sends
         those instead of its own pb::Writer ones, so session.cpp's parsing
         is tested on the real encoding.
check:   decodes every message the head unit sent in that session
         (<dir>/hu/index.txt + <nnn>.bin, written by the test) as the
         message type its channel and id stand for. Each one must parse,
         have all required fields, have no field or enum value the .proto
         doesn't know (a wrong field number shows up as one), and encode
         back to the same bytes. Every type the head unit can send must
         have been seen at least once.
"""

import importlib
import subprocess
import sys
from pathlib import Path

# Our channel numbers: src/android_auto/session.h.
CH_CONTROL, CH_INPUT, CH_SENSOR = 0, 1, 2
MEDIA_CHANNELS = (3, 4, 5, 6, 7)
FLAG_CONTROL = 0x04

CTRL = "aap_protobuf.service.control.message."
MEDIA = "aap_protobuf.service.media."
SENSOR = "aap_protobuf.service.sensorsource.message."

# What the head unit sends, by channel: message id -> type. Ids 1 and 3 on
# the control channel (version, TLS) are raw bytes, not protobuf.
CONTROL_TYPES = {
    4: CTRL + "AuthResponse",
    6: CTRL + "ServiceDiscoveryResponse",
    11: CTRL + "PingRequest",
    12: CTRL + "PingResponse",
    14: CTRL + "NavFocusNotification",
    15: CTRL + "ByeByeRequest",
    16: CTRL + "ByeByeResponse",
    19: CTRL + "AudioFocusNotification",
}
CONTROL_RAW = {1, 3}
CHANNEL_OPEN_RESPONSE = (8, CTRL + "ChannelOpenResponse")
MEDIA_TYPES = {
    0x8003: MEDIA + "shared.message.Config",
    0x8004: MEDIA + "source.message.Ack",
    0x8006: MEDIA + "source.message.MicrophoneResponse",
    0x8008: MEDIA + "video.message.VideoFocusNotification",
}
SENSOR_TYPES = {
    0x8002: SENSOR + "SensorResponse",
    0x8003: SENSOR + "SensorBatch",
}
INPUT_TYPES = {0x8003: MEDIA + "sink.message.KeyBindingResponse"}

# Not expected in a test session: the head unit only sends it on stop.
NOT_IN_SESSION = {CTRL + "ByeByeRequest"}


def compile_protos(out: Path, proto_root: Path) -> None:
    """Run protoc on every .proto under proto_root into out."""
    out.mkdir(parents=True, exist_ok=True)
    protos = sorted(str(p.relative_to(proto_root)) for p in proto_root.rglob("*.proto"))
    subprocess.run(
        [sys.executable, "-m", "grpc_tools.protoc", f"-I{proto_root}", f"--python_out={out}", *protos],
        check=True,
    )


def load_types(out: Path):
    """Import every generated module; return a function: full name -> message class."""
    from google.protobuf import descriptor_pool, message_factory

    sys.path.insert(0, str(out))
    for mod in sorted(out.rglob("*_pb2.py")):
        importlib.import_module(".".join(mod.relative_to(out).with_suffix("").parts))
    pool = descriptor_pool.Default()
    return lambda name: message_factory.GetMessageClass(pool.FindMessageTypeByName(name))


def prepare(d: Path, proto_root: Path) -> None:
    """Compile the protos and write the phone's messages."""
    compile_protos(d / "py", proto_root)
    msg = load_types(d / "py")
    phone = d / "phone"
    phone.mkdir(parents=True, exist_ok=True)

    def put(name: str, m) -> None:
        (phone / f"{name}.bin").write_bytes(m.SerializeToString())

    put("sd_request", msg(CTRL + "ServiceDiscoveryRequest")(label_text="Android Auto", device_name="Test Phone"))
    put("channel_open", msg(CTRL + "ChannelOpenRequest")(priority=-1, service_id=3))
    put("ping", msg(CTRL + "PingRequest")(timestamp=123))
    put("pong", msg(CTRL + "PingResponse")(timestamp=2000))
    put("byebye", msg(CTRL + "ByeByeRequest")(reason=1))  # USER_SELECTION
    put("audio_focus", msg(CTRL + "AudioFocusRequest")(audio_focus_type=1))  # GAIN
    put("nav_focus", msg(CTRL + "NavFocusRequestNotification")(focus_type=2))  # PROJECTED
    put("setup", msg(MEDIA + "shared.message.Setup")(type=3))  # VIDEO_H264_BP
    put("start", msg(MEDIA + "shared.message.Start")(session_id=7, configuration_index=0))
    put("video_focus", msg(MEDIA + "video.message.VideoFocusRequestNotification")(mode=1, reason=0))
    put("mic_request", msg(MEDIA + "source.message.MicrophoneRequest")(open=True))
    put("sensor_driving", msg(SENSOR + "SensorRequest")(type=13, min_update_period=0))
    put("sensor_night", msg(SENSOR + "SensorRequest")(type=10, min_update_period=1000))
    put("sensor_speed", msg(SENSOR + "SensorRequest")(type=3, min_update_period=100))
    put("key_binding", msg(MEDIA + "sink.message.KeyBindingRequest")(keycodes=[19, 20, 23, 65536]))
    print(f"check_aa_proto: {len(list(phone.iterdir()))} phone messages written")


def type_for(ch: int, flags: int, mid: int) -> str | None:
    """Return the message type the head unit sends on ch with id mid; None for raw bytes."""
    if flags & FLAG_CONTROL and mid == CHANNEL_OPEN_RESPONSE[0]:
        return CHANNEL_OPEN_RESPONSE[1]
    if ch == CH_CONTROL:
        if mid in CONTROL_RAW:
            return None
        table = CONTROL_TYPES
    elif ch == CH_SENSOR:
        table = SENSOR_TYPES
    elif ch == CH_INPUT:
        table = INPUT_TYPES
    elif ch in MEDIA_CHANNELS:
        table = MEDIA_TYPES
    else:
        table = {}
    if mid not in table:
        raise KeyError(f"channel {ch} id 0x{mid:04x}: not a message the head unit should send")
    return table[mid]


def unknown_fields(m, path: str = "") -> list[str]:
    """Fields (or enum values) in m and its sub-messages that the .proto doesn't know."""
    from google.protobuf.unknown_fields import UnknownFieldSet

    found = [f"{path or '.'}: field {f.field_number}" for f in UnknownFieldSet(m)]
    for fd, value in m.ListFields():
        if fd.message_type is None:
            continue
        items = value if fd.is_repeated else [value]
        for i, sub in enumerate(items):
            found += unknown_fields(sub, f"{path}.{fd.name}[{i}]")
    return found


def check_one(cls, data: bytes) -> list[str]:
    """Problems with one message: parse, required fields, unknown fields, re-encoding."""
    m = cls()
    try:
        m.ParseFromString(data)
    except Exception as e:  # any parse failure is the finding
        return [f"does not parse: {e}"]
    problems = [f"missing required {x}" for x in m.FindInitializationErrors()]
    problems += [f"unknown {x}" for x in unknown_fields(m)]
    if not problems and m.SerializeToString(deterministic=True) != data:
        problems.append("encodes differently: " + m.SerializeToString(deterministic=True).hex())
    return problems


def check(d: Path) -> int:
    """Decode every head unit message of the session; 0 if all are right."""
    from google.protobuf import text_format

    msg = load_types(d / "py")
    seen: dict[str, int] = {}
    failures = 0
    for line in (d / "hu" / "index.txt").read_text().splitlines():
        seq, ch, flags, mid = (int(x, 0) for x in line.split())
        data = (d / "hu" / f"{seq:04d}.bin").read_bytes()
        name = type_for(ch, flags, mid)
        if name is None:
            continue
        seen[name] = seen.get(name, 0) + 1
        problems = check_one(msg(name), data)
        if problems:
            failures += 1
            m = msg(name)()
            m.MergeFromString(data)
            print(f"FAIL {seq:04d} ch {ch} id 0x{mid:04x} {name}: {'; '.join(problems)}")
            print("  " + text_format.MessageToString(m).replace("\n", "\n  "))
    expected = {
        *CONTROL_TYPES.values(),
        CHANNEL_OPEN_RESPONSE[1],
        *MEDIA_TYPES.values(),
        *SENSOR_TYPES.values(),
        *INPUT_TYPES.values(),
    } - NOT_IN_SESSION
    for name in sorted(expected - seen.keys()):
        failures += 1
        print(f"FAIL never sent in the test session: {name}")
    total = sum(seen.values())
    print(f"check_aa_proto: {total} head unit messages of {len(seen)} types checked, {failures} failed")
    return 1 if failures else 0


def main() -> int:
    """Entry point: prepare|check <dir> <proto_root>."""
    if len(sys.argv) != 4 or sys.argv[1] not in ("prepare", "check"):
        print(__doc__)
        return 2
    d, proto_root = Path(sys.argv[2]), Path(sys.argv[3])
    if sys.argv[1] == "prepare":
        prepare(d, proto_root)
        return 0
    sys.path.insert(0, str(d / "py"))
    return check(d)


if __name__ == "__main__":
    sys.exit(main())
