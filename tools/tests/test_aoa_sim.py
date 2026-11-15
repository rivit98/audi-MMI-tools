import aoa_sim
from aoa_sim import AckGate, Controls, IoMediaModel


def run(model, secs, fps=25.0, idr_every=0, sync_gives_idr=True):
    """A phone at fps against the model; returns the events seen."""
    events, want_idr, k = [], False, 0
    t, step = 0.0, 0.01
    next_frame = 0.0
    while t <= secs:
        if t >= next_frame:
            idr = k == 0 or want_idr or (idr_every and k % idr_every == 0)
            got = model.frame(idr, t)
            if idr and got == "ok":
                want_idr = False
            if got == "wait_idr":
                want_idr = sync_gives_idr
            events.append(got)
            k += 1
            next_frame += 1 / fps
        got = model.tick(t)
        if got:
            events.append(got)
            if got == "sync":
                want_idr = True
            if got == "eio":
                return events
        t += step
    return events


def test_sample_size_matches_the_mmi():
    assert aoa_sim.sample_size(262144) == 266240   # the car run's 256 KB config
    assert aoa_sim.sample_size(131072) == 135168
    assert aoa_sim.sample_size(1000) == 8192


def test_256k_frames_overrun_the_ring_without_catch_up():
    model = IoMediaModel(262144, 0.0, catch_up=False)
    assert model.ring == 31
    events = run(model, 120)
    assert events[-1] == "eio"
    assert model.behind() > 31


def test_256k_frames_catch_up_instead_of_the_error():
    model = IoMediaModel(262144, 0.0, catch_up=True)
    events = run(model, 120)
    assert "eio" not in events
    assert model.catch_ups >= 3
    assert events.count("sync") == model.catch_ups
    assert model.behind_max <= model.catch_up_at + 2


def test_128k_frames_keep_up():
    model = IoMediaModel(131072, 0.0, catch_up=False)
    events = run(model, 120)
    assert "eio" not in events
    assert model.behind_max <= 1


def test_waits_for_an_idr_first():
    model = IoMediaModel(65536, 0.0)
    assert model.frame(False, 0.0) == "wait_idr"
    assert model.frame(True, 0.04) == "ok"


def test_started_model_needs_no_idr():
    model = IoMediaModel(65536, 0.0, started=True)
    assert model.frame(False, 0.04) == "ok"


def test_gap_is_filled_up_to_the_clock():
    model = IoMediaModel(65536, 0.0)
    model.frame(True, 0.0)
    model.tick(1.0)
    assert model.frames == 25         # 26 due: one short


def test_ack_gate_passes_acks_at_once():
    gate = AckGate()
    assert gate.add(b"a", 0.0) == [b"a"]


def test_ack_gate_holds_then_releases_in_order():
    gate = AckGate()
    gate.hold(0.0, secs=3.0)
    assert gate.add(b"a", 1.0) == []
    assert gate.add(b"b", 2.0) == []
    assert gate.held() == 2
    assert gate.due(3.0) == [b"a", b"b"]


def test_slow_acks_go_out_at_the_slow_rate():
    gate = AckGate(slow=True)
    out = []
    for i in range(50):
        out += gate.add(bytes([i]), i * 0.04)   # 25 a second for 2 s
    assert 9 <= len(out) <= 11                 # 5 a second
    assert gate.held() == 50 - len(out)


def test_controls_take_known_switches_only():
    c = Controls()
    c.update({"iomedia": True, "catch_up": False, "bogus": True})
    assert c.iomedia and not c.catch_up
    assert not hasattr(c, "bogus")
