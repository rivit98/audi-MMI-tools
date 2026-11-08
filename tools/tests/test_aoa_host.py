import os

import aoa_host
import pytest
import usb.core
from aoa_host import Handshakes


class FakeDevice:
    idVendor, idProduct = 0x2717, 0xFF48

    def __init__(self, fail_at_request=None):
        self.fail_at_request = fail_at_request
        self.requests = []

    def ctrl_transfer(self, _type, request, _value, _index, _data, timeout):
        self.requests.append(request)
        if request == self.fail_at_request:
            raise usb.core.USBError("Pipe error")
        return [2, 0] if request == aoa_host.AOA_GET_PROTOCOL else None


def add_sysfs_device(root, name, busnum, devnum):
    d = root / name
    d.mkdir()
    (d / "busnum").write_text(f"{busnum}\n")
    (d / "devnum").write_text(f"{devnum}\n")


def test_bus_devices_lists_devices_and_skips_interfaces(tmp_path):
    add_sysfs_device(tmp_path, "3-2", 3, 16)
    (tmp_path / "3-2:1.0").mkdir()      # an interface has no busnum
    assert aoa_host.bus_devices(tmp_path) == {("3", "16")}


def test_bus_devices_differ_when_a_phone_reenumerates(tmp_path):
    add_sysfs_device(tmp_path, "3-2", 3, 16)
    before = aoa_host.bus_devices(tmp_path)
    (tmp_path / "3-2" / "devnum").write_text("17\n")
    assert aoa_host.bus_devices(tmp_path) != before


def test_a_changed_bus_restarts_the_process(monkeypatch):
    execs = []
    monkeypatch.setattr(aoa_host, "BUS_AT_START", {("3", "16")})
    monkeypatch.setattr(aoa_host, "bus_devices", lambda: {("3", "17")})
    monkeypatch.setattr(aoa_host.os, "execv", lambda path, argv: execs.append((path, argv)))
    monkeypatch.setattr(aoa_host.sys, "argv", ["aoa_host.py", "--no-display"])
    aoa_host.restart_if_bus_changed()
    assert execs == [(aoa_host.sys.executable, [aoa_host.sys.executable, "aoa_host.py", "--no-display"])]


def test_first_handshake_waits_after_the_device_appears():
    h = Handshakes(first_s=4, retry_s=5, tries=5)
    assert not h.due("phone", 100)
    assert not h.due("phone", 103)
    assert h.due("phone", 104)


def test_failed_handshake_is_retried_until_tries_run_out():
    h = Handshakes(first_s=0, retry_s=5, tries=2)
    assert h.due("phone", 0)
    assert h.tried("phone", 0) == 1
    assert not h.due("phone", 4)
    assert h.due("phone", 5)
    assert h.tried("phone", 5) == 2
    assert not h.due("phone", 1000)


def test_a_new_bus_address_starts_afresh():
    h = Handshakes(first_s=0, retry_s=5, tries=1)
    assert h.due("old address", 0)
    h.tried("old address", 0)
    assert not h.due("old address", 100)
    assert h.due("new address", 100)


def test_switch_succeeds_with_start_as_the_last_request():
    dev = FakeDevice()
    assert aoa_host.try_switch(dev) is None
    assert dev.requests[-1] == aoa_host.AOA_START


def test_refused_protocol_query_is_reported():
    assert "Pipe error" in aoa_host.try_switch(FakeDevice(fail_at_request=aoa_host.AOA_GET_PROTOCOL))


def test_failure_during_the_strings_is_reported_not_raised():
    assert "Pipe error" in aoa_host.try_switch(FakeDevice(fail_at_request=aoa_host.AOA_SEND_STRING))


def test_screen_messages_cross_the_pipe_in_order():
    read_end, write_end = os.pipe()
    link = aoa_host.ScreenLink(write_end)
    link.send("h264", b"\0\1\2")
    link.send("link", state="connected", detail="x")
    os.close(write_end)
    assert list(aoa_host.read_messages(read_end)) == [
        ({"kind": "h264"}, b"\0\1\2"),
        ({"kind": "link", "state": "connected", "detail": "x"}, b""),
    ]


def test_the_usb_process_stops_when_the_window_is_gone():
    read_end, write_end = os.pipe()
    os.close(read_end)
    with pytest.raises(SystemExit):
        aoa_host.ScreenLink(write_end).send("h264", b"x")
    os.close(write_end)


def test_a_pause_is_logged_once_and_when_it_ends(capsys):
    gaps = aoa_host.FrameGaps(gap_s=1)
    gaps.frame(10)
    gaps.idle(10.5)
    assert capsys.readouterr().out == ""
    gaps.idle(11)
    gaps.idle(12)
    assert capsys.readouterr().out.count("no frames for") == 1
    gaps.frame(13)
    assert "frames back after a pause of 3.0 s" in capsys.readouterr().out


def test_stream_end_says_why_and_how_long_since_the_last_frame(capsys):
    gaps = aoa_host.FrameGaps()
    gaps.frame(10)
    gaps.ended("VIDEO_STOP from the phone", 10.25)
    out = capsys.readouterr().out
    assert "VIDEO_STOP from the phone" in out
    assert "last frame 0.2 s before" in out
    gaps.idle(100)
    assert capsys.readouterr().out == ""


@pytest.mark.parametrize("status, counts", [
    (aoa_host.VIDEO_SHOWN, (1, 0, 0)),
    (aoa_host.VIDEO_BAD, (0, 1, 0)),
    (aoa_host.VIDEO_DROPPED, (0, 0, 1)),
])
def test_an_ack_counts_one_picture(status, counts):
    # proto.h VIDEO_ACKS: shown u16, bad u16, dropped u16
    shown, bad, dropped = counts
    assert aoa_host.ack_frame(status) == bytes([0x0D, 6, 0, shown, 0, bad, 0, dropped, 0])
