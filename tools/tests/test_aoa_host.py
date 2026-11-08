import usb.core

import aoa_host
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
