"""plot_stream_perf.py's log parsing, on lines in the old and the new formats."""
import plot_stream_perf as p

OLD = """\
[ 10143 ms] cpu: 10.0 s: self 9% (max 36%), unit 38% (max 61%), others 29% | main 9%
[ 26049 ms] h264: config: 25 fps, frames up to 65536 bytes (padded to 69632), SPS 18 / PPS 4 bytes
[ 32837 ms] h264: 10.0 s: 129 frames (12.9 fps), 119 skips, 0 refused; io-media 264 reads, 231 waited (max 85 ms), lead max 1 frames; lag 491/532/766 ms (24 TIME), position 9.3 s
[343885 ms] perf: window 10.2 s: 42 shown 4.1 fps, 6 dropped, 0 bad | ms avg/max: rx 5/6 wait 12/58 decode 66/68 render 23/24 copy 112/184 flip 19/43, latency 239/331 | JPEG 42 x 52 KB, decode 66/68, phone 67/85 | vfb 916 KB 394 rows, 1 full | main busy 99%, cpu 87%
[354022 ms] cpu: 10.1 s: self 95% (max 99%), unit 99% (max 100%), others 4% | main 91%, usbd 3% - HIGH
[402705 ms] h264: io-media error: ERROR event: Input/output error
[403059 ms] h264: tx H264_STATUS REFUSED
"""

NEW = """\
[ 45925 ms] h264: 10.0 s: 130 frames (13.0 fps), 118 skips, 0 refused; io-media 512 reads, 0 waited (max 63 ms), lead -2/0/1 frames; lag 296/324/461 ms (24 TIME), position 9.3 s; evicted 7, far 0, eof 1, unblocked 2; reader 120 frames behind, ring 60 frames
[ 46000 ms] h264: 10.0 s: 250 frames (25.0 fps), 0 skips, 0 refused; io-media 512 reads, 0 waited (max 0 ms), lead -2/0/1 frames; lag 296/324/461 ms (24 TIME, 1 not kept), position 9.3 s; evicted 0, far 0, eof 0, unblocked 0; reader 2 frames behind, ring 62 frames; data 4012 kbit/s, frames avg 19 / max 131 KB, 1 IDR
[ 47000 ms] cpu: 10.0 s: self 9% (max 36%), unit 38% (max 61%), others 29% | main 9%, usbd 2% | mem free 141 MB, heap 3.2 MB - HIGH
[ 48000 ms] rx STREAM_SETTINGS len=40: up=123 format=H264 sending=H264 max_fps=25 bit_rate_kbit=4000
[ 50000 ms] perf: window 10.0 s: 50 shown 5.0 fps, 0 dropped, 0 bad | ms avg/max: rx 5/6 wait 12/58 decode 66/68 render 23/24 copy 30/40 flip 19/43, latency 239/331 | JPEG 50 x 52 KB, decode 66/68, phone 67/85 | vfb 916 KB 394 rows, 0 full | main busy 70%, cpu 60% | cap 5 fps: 48 delayed 80/120, 50 acks held 90/110
[ 50010 ms] perf: HIGH main load: main loop 85% busy over 10.0 s of JPEG (0 more not logged)
[ 50020 ms] h264: tx H264_STATUS REFUSED (reason: io-media error)
[ 50030 ms] h264: WARNING lag median 1200 ms, over 1000 ms (0 more not logged)
"""


def run_of(tmp_path, text, name="audi_stream_receiver.log"):
    log = tmp_path / name
    log.write_text(text)
    return p.parse_log(log)


def test_old_format(tmp_path):
    r = run_of(tmp_path, OLD)
    assert len(r.cpu) == 2
    assert r.cpu[1]["main"] == 91
    assert r.cpu[1]["high"]
    assert r.h264[0]["frames"] == 129
    assert r.h264[0]["lag_p50"] == 532
    assert r.h264[0]["evicted"] is None
    assert r.jpeg[0]["shown"] == 42
    assert r.jpeg[0]["busy"] == 99
    assert r.jpeg[0]["delayed"] is None
    kinds = [k for _, k, _ in r.events]
    assert kinds == ["config", "io-media error", "REFUSED"]
    assert any("1 io-media errors" in s for s in p.summary(r))


def test_new_format(tmp_path):
    r = run_of(tmp_path, NEW)
    assert r.h264[0]["evicted"] == 7
    assert r.h264[0]["behind"] == 120
    assert r.h264[0]["waited"] == 0
    assert r.jpeg[0]["delayed"] == 48
    assert r.jpeg[0]["held"] == 50
    assert r.h264[1]["kbit"] == 4012
    assert r.h264[1]["max_kb"] == 131
    assert r.h264[1]["times"] == 24
    assert r.h264[0]["kbit"] is None
    assert r.cpu[0]["free_mb"] == 141
    assert r.cpu[0]["heap_mb"] == 3.2
    assert r.cpu[0]["main"] == 9
    assert r.cpu[0]["high"]
    kinds = [k for _, k, _ in r.events]
    assert kinds == ["settings", "HIGH load", "REFUSED", "warning"]
    text = "\n".join(p.summary(r))
    assert "4012 kbit/s" in text
    assert "unit free 141 -> 141 MB" in text
    p.plot(r, tmp_path / "new.png", "new")
    assert (tmp_path / "new.png").stat().st_size > 10000


def test_csv_pairs_and_plot(tmp_path):
    (tmp_path / "audi_stream_receiver.prev.log").write_text(OLD)
    (tmp_path / "audi_stream_receiver_out.log").write_text(OLD)
    csv = tmp_path / "audi_stream_perf.prev.csv"
    csv.write_text(
        "t_ms,id,status,w,h,bytes,rx_us,wait_us,decode_us,render_us,copy_us,flip_us,"
        "vfb_bytes,vfb_rows,vfb_full,latency_us,phone_ms\n"
        "333679,1,2,800,390,52299,4000,0,0,0,0,0,0,0,0,0,48\n"
        "333800,2,0,800,390,52201,5000,1000,66000,23000,25000,3000,1,1,0,120000,11\n"
        "334000,3,0,800,390,52201,5000,1000,66000,23000,25000,3000,1,1,0,120000,11\n"
        "3340")                                 # cut short by a pulled card
    logs = p.find_logs(tmp_path)
    assert [x.name for x in logs] == ["audi_stream_receiver.prev.log"]
    assert p.csv_for(logs[0]) == csv
    pics = p.parse_csv(csv)
    assert len(pics) == 2
    assert pics[0]["decode"] == 66.0
    assert pics[0]["cap"] == 0.0
    assert p.main([str(tmp_path)]) == 0
    assert (tmp_path / "audi_stream_receiver.prev.png").stat().st_size > 10000
