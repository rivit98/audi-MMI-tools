#!/usr/bin/env python3
"""Decode a raw capture of /dev/ipc/ch5, the IOC's vehicle-signal channel.

Usage: ch5dump.py <capture.bin> [--all]

ndr reads this channel and feeds the navigation (docs/vehicle-data.md). A
capture is the bytes read from the channel, e.g. sysinfo_dump.sh's
`ipc/ch5.bin` or `cat /dev/ipc/ch5 > file` on the unit.

Record layout (ndr CProcessCh5::interpretBuffer @ 0x08063208, P1001 rev 51):
    tag u8, subtag u8, len u16 LE, payload[len], ts u16 LE (IOC ms clock)

Prints a per-tag summary (count, period, decoded first records); --all
prints every record.
"""

import collections
import itertools
import struct
import sys

# tag -> handler name in ndr (interpretBuffer's switch)
TAGS = {
    0x01: 'GYRO',
    0x02: 'RIZ',
    0x03: 'REV',
    0x04: 'STR_ANGLE',
    0x05: 'ExternalGYRO',
    0x06: 'BrakeLightESP',
    0x07: 'TractionBrake',
    0x08: 'OutsideTemp',
    0x09: 'RainSens',
    0x0A: 'Wiper',
    0x0B: 'CURVETRONIC',
    0x0C: 'RevGear',
    0x0D: 'SpeedoPulse',
    0x0E: 'GearPosition',
    0x0F: 'TEMPERATURE',
    0x10: '3DAcceleration',
    0x11: '3DAccelerationHr',
    0xF0: 'TIMEDATE',
    0xFF: 'PROTOCOL',
}

USAGE_ARGS = 2  # program name + capture path

Record = collections.namedtuple('Record', 'offset tag subtag payload ts')


def records(data):
    """Yield Records; stops at a truncated tail."""
    i = 0
    while i + 4 <= len(data):
        tag, sub = data[i], data[i + 1]
        (ln,) = struct.unpack_from('<H', data, i + 2)
        end = i + 4 + ln + 2
        if end > len(data):
            return
        (ts,) = struct.unpack_from('<H', data, i + 4 + ln)
        yield Record(i, tag, sub, data[i + 4 : i + 4 + ln], ts)
        i = end


def _u16s(p):
    return struct.unpack_from(f'<{len(p) // 2}H', p)


def _wheels(mask, values, fmt):
    return ' '.join(
        f'w{n}={fmt(v)}' if mask & (1 << n) else f'w{n}=-' for n, v in enumerate(values[:4])
    )


def _riz(sub, p):
    # 4 x u16 pulse counters; subtag = valid-wheel mask (processRIZ 0x08061268)
    return _wheels(sub, _u16s(p[:8]), str)


def _rev(sub, p):
    # 1 byte per wheel; hfs: reverse = (byte == 2) (hfs processREV 0x0805fa20)
    return _wheels(sub, p, lambda v: {1: 'fwd', 2: 'back'}.get(v, str(v)))


def _temperature(sub, p):
    # 8 x s16; channel 1 = degC*10, valid if subtag bit 1 (hfs 0x0805f7ac)
    ch = struct.unpack_from('<8h', p)
    c1 = f'{(ch[1] + 5) // 10} degC' if sub & 2 else 'n/a'
    return f'channels {list(ch)}  ch1 {c1}'


DECODERS = {
    0x02: _riz,
    0x03: _rev,
    0x0C: lambda sub, p: f'reverse gear byte {p[0]}',
    0x0D: lambda sub, p: f'pulses {_u16s(p)[0]}',
    0x0E: lambda sub, p: f'gear byte (payload[9]) {p[9]}',
    0x0F: _temperature,
}


def describe(rec):
    """Return the decoded fields of a record, or '' if the tag isn't decoded."""
    dec = DECODERS.get(rec.tag)
    try:
        return dec(rec.subtag, rec.payload) if dec else ''
    except (IndexError, struct.error):
        return '(short payload)'


def name_of(tag):
    """Return ndr's handler name for a tag."""
    return TAGS.get(tag, f'tag_0x{tag:02x}')


def summary(groups):
    """Print one block per (tag, subtag, len) group."""
    for (tag, sub, ln), recs in groups.items():
        # the ts clock is u16 ms, so differences are taken mod 65536
        deltas = sorted((b.ts - a.ts) & 0xFFFF for a, b in itertools.pairwise(recs))
        period = f' period ~{deltas[len(deltas) // 2]} ms,' if deltas else ''
        distinct = len({r.payload for r in recs})
        print(
            f'tag 0x{tag:02x} {name_of(tag):16} sub {sub} len {ln:2}: '
            f'{len(recs):3} records,{period} {distinct} distinct payload(s)'
        )
        for r in recs[:3]:
            print(f'    ts={r.ts:5}  {r.payload.hex(" ")}  {describe(r)}')


def main(argv):
    """Parse the capture named in argv[1] and print it."""
    if len(argv) < USAGE_ARGS:
        sys.exit(__doc__)
    with open(argv[1], 'rb') as f:
        data = f.read()
    show_all = '--all' in argv[2:]
    groups = {}
    used = 0
    for rec in records(data):
        used = rec.offset + 4 + len(rec.payload) + 2
        if show_all:
            print(
                f'{rec.offset:6}  ts={rec.ts:5}  {name_of(rec.tag):16} sub {rec.subtag} '
                f'len {len(rec.payload):2}  {rec.payload.hex(" ")}  {describe(rec)}'
            )
        groups.setdefault((rec.tag, rec.subtag, len(rec.payload)), []).append(rec)
    tail = '' if used == len(data) else ' (tail not a whole record)'
    print(f'{used} of {len(data)} bytes parsed{tail}')
    summary(groups)


if __name__ == '__main__':
    main(sys.argv)
