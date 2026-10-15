"""
Tests for XAL_WATCHMODE_READ_LEASE (the FIEMAP read-lease mode).

These drive the ``xal_lease_hold`` helper, which opens the FIEMAP backend in read-lease mode,
indexes, and holds the extents of one file with xal_extents_acquire() until signalled. A test
can so open the file for writing meanwhile and check that the writer waits, that the extents still
hold the file's data, and that the file is mapped again afterwards. The helper runs under sudo,
which gives it CAP_LEASE for files the test user owns.
"""

import re
import time
from pathlib import Path

import pytest

READY_RE = re.compile(r"LEASE_READY pid=(\d+) byte=(\d+) extents=(\d+)")
REACQUIRED_RE = re.compile(r"REACQUIRED byte=(\d+) extents=(\d+)")
EXTENT_RE = re.compile(r"EXTENT 0 byte=(\d+) bytes=(\d+) flag=0x([0-9a-f]+)")

XAL_EXTENT_SHARED = 0x2  # enum xal_extent_flag
ENOTSUP = 95


@pytest.fixture(autouse=True)
def _restore_lease_break_time(cijoe):
    """Some tests change the system-wide fs.lease-break-time; restore it even if they fail."""

    saved = _capture(cijoe, "cat /proc/sys/fs/lease-break-time")
    yield
    cijoe.run(f"sudo sysctl -qw fs.lease-break-time={saved}")


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    """Run 'cmd', capturing stdout to an artifact file, and return it stripped."""

    out = _artifacts(cijoe) / "lease_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _require_fixture(cijoe):
    """Resolve (dev_path, mountpoint) and assert the mount comes from dev_path."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    src = _capture(cijoe, f"findmnt -n -o SOURCE {mountpoint}")
    if not src:
        pytest.skip(f"nothing is mounted at {mountpoint}")
    assert src == dev_path, f"{mountpoint} is mounted from {src!r}, not {dev_path!r}"

    return dev_path, mountpoint


def _fill(cijoe, path, char, nbytes, sync=True):
    cijoe.run(f"sh -c \"printf '%{nbytes}s' | tr ' ' {char} > {path}\"")
    if sync:
        cijoe.run("sync")


def _raw(cijoe, dev_path, byte, nbytes=4) -> str:
    """The first nbytes at the device byte offset, read with O_DIRECT past the page cache."""

    assert byte % 4096 == 0, f"byte offset {byte} is not 4 KiB aligned"
    return _capture(
        cijoe,
        f"sudo dd if={dev_path} bs=4096 skip={byte // 4096} count=1 iflag=direct status=none "
        f"| head -c {nbytes}",
    )


def _background(cijoe, name, script) -> Path:
    """Run a shell script in the background; return the path of its log."""

    art = _artifacts(cijoe)
    (art / f"{name}.sh").write_text(script)
    log = art / f"{name}.log"
    cijoe.run(f"rm -f {log}")
    cijoe.run(f"sh -c 'nohup sh {art / name}.sh > {log} 2>&1 &'")
    return log


def _wait_for(path: Path, regex, timeout):
    for _ in range(int(timeout / 0.1)):
        if path.exists():
            m = regex.search(path.read_text())
            if m:
                return m
        time.sleep(0.1)
    text = path.read_text() if path.exists() else "(no log)"
    raise AssertionError(f"{regex.pattern} not seen in {path} within {timeout}s:\n{text}")


def _start_hold(cijoe, dev_path, mountpoint, path, option=""):
    """Start xal_lease_hold on path; return (pid, byte, log) once it holds the extents."""

    log = _artifacts(cijoe) / "lease_hold.log"
    cijoe.run(f"sudo rm -f {log}")
    cijoe.run(
        f"sudo sh -c 'nohup xal_lease_hold {dev_path} {mountpoint} {path} {option} > {log} 2>&1 & "
        f"echo backgrounded'"
    )
    m = _wait_for(log, READY_RE, 30)
    return int(m.group(1)), int(m.group(2)), log


def _stop_hold(cijoe, pid):
    cijoe.run(f"sudo kill {pid}")
    for _ in range(100):
        err, _ = cijoe.run(f"sudo kill -0 {pid}")
        if err:
            return
        time.sleep(0.1)
    raise AssertionError(f"xal_lease_hold pid {pid} did not exit")


def _probe(cijoe, dev_path, mountpoint, path) -> str:
    return _capture(cijoe, f"sudo xal_lease_hold {dev_path} {mountpoint} {path} --probe")


def _timed(cijoe, name, cmd) -> Path:
    """Run cmd in the background; its log gets took_ms=<ms> once it is done."""

    return _background(
        cijoe,
        name,
        f"start=$(date +%s%N)\n{cmd}\necho took_ms=$(( ($(date +%s%N) - start) / 1000000 ))\n",
    )


TOOK_RE = re.compile(r"took_ms=(\d+)")


def _writer_waits_for_reader(cijoe, name, option=""):
    """While the extents are held, a writer's open() waits and the extents keep the file's data;
    once they are released the writer goes ahead, and the file is mapped again after it."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/{name}"
    _fill(cijoe, path, "A", 4096)

    pid, byte, log = _start_hold(cijoe, dev_path, mountpoint, path, option)
    try:
        assert _raw(cijoe, dev_path, byte) == "AAAA", "held extent does not hold the file's data"

        # fsync, so that without the lease the write would reach the device within the second
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        time.sleep(1)

        assert "took_ms" not in (writer.read_text() if writer.exists() else ""), (
            "the writer was not held back by the lease"
        )
        assert _raw(cijoe, dev_path, byte) == "AAAA", "the held extent changed under the reader"

        cijoe.run(f"sudo kill -USR1 {pid}")
        took = int(_wait_for(writer, TOOK_RE, 10).group(1))
        assert took >= 900, f"writer took {took} ms, less than the 1 s the extents were held"

        m = _wait_for(log, REACQUIRED_RE, 30)
        assert _raw(cijoe, dev_path, int(m.group(1)), 1) == "B", "remapped extent lacks the write"
    finally:
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {path}")


def test_lease_writer_waits_for_reader(cijoe):
    _writer_waits_for_reader(cijoe, "xal_lease_fence")


def test_lease_acquired_on_exited_thread(cijoe):
    """The break reaches xal even when the thread that took the lease has exited."""

    _writer_waits_for_reader(cijoe, "xal_lease_thread", "--thread")


def test_lease_other_file_not_delayed(cijoe):
    """A writer waiting on one held file does not hold up the break of another file's lease,
    and truncate(2) on a path breaks a lease as open() does."""

    dev_path, mountpoint = _require_fixture(cijoe)
    held, idle = f"{mountpoint}/xal_lease_held", f"{mountpoint}/xal_lease_idle"
    _fill(cijoe, held, "A", 4096)
    _fill(cijoe, idle, "A", 4096)

    pid, _, _ = _start_hold(cijoe, dev_path, mountpoint, held, f"--idle {idle}")
    try:
        _timed(cijoe, "lease_held_writer", f"printf B | dd of={held} conv=notrunc status=none")
        time.sleep(0.5)

        # truncate(2), which waits for the lease; coreutils truncate opens with O_NONBLOCK
        truncater = _timed(
            cijoe,
            "lease_idle_truncate",
            f"python3 -c 'import os; os.truncate(\"{idle}\", 0)' && echo truncated",
        )
        took = int(_wait_for(truncater, TOOK_RE, 10).group(1))
        assert "truncated" in truncater.read_text(), truncater.read_text()
        assert _capture(cijoe, f"stat -c %s {idle}") == "0", "truncate did not take effect"
        assert took < 2000, f"truncate of a file without readers took {took} ms"
    finally:
        cijoe.run(f"sudo kill -USR1 {pid}")
        _stop_hold(cijoe, pid)

    cijoe.run(f"rm -f {held} {idle}")


def test_lease_refused_while_open_for_writing(cijoe):
    """A file held open for writing gets no lease, so its extents are not handed out."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_refused"
    _fill(cijoe, path, "A", 4096)

    fifo = _artifacts(cijoe) / "lease_refused.fifo"
    cijoe.run(f"rm -f {fifo}")
    cijoe.run(f"mkfifo {fifo}")
    holder = _background(
        cijoe, "lease_holder", f"exec 3>>{path}\necho opened\nread line < {fifo}\necho closed\n"
    )
    _wait_for(holder, re.compile("opened"), 10)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=-11" in out, f"lease granted on a file open for writing:\n{out}"

    cijoe.run(f"sh -c 'echo done > {fifo}'")
    _wait_for(holder, re.compile("closed"), 10)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, f"lease refused after the writer closed:\n{out}"

    cijoe.run(f"rm -f {path} {fifo}")


def test_lease_maps_dirty_data_after_writeback(cijoe):
    """Data still in the page cache is written back before mapping, so the extents have a disk
    address and hold the data."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_dirty"
    _fill(cijoe, path, "C", 65536, sync=False)

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, out
    m = EXTENT_RE.search(out)
    assert m, f"no extent reported:\n{out}"
    byte, flag = int(m.group(1)), int(m.group(3), 16)

    assert not flag & ~XAL_EXTENT_SHARED, f"extent flagged other than shared:\n{out}"
    assert byte, f"extent without a disk address:\n{out}"
    assert _raw(cijoe, dev_path, byte) == "CCCC", "extent does not hold the written data"

    cijoe.run(f"rm -f {path}")


def test_lease_reports_unwritten(cijoe):
    """Unwritten extents are handed out with XAL_EXTENT_UNWRITTEN, as XAL_WATCHMODE_NONE does."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_unwritten"
    cijoe.run(f"fallocate -l 1M {path}")
    cijoe.run(f"sh -c \"printf '%4096s' | tr ' ' D | dd of={path} conv=notrunc,fsync status=none\"")

    out = _probe(cijoe, dev_path, mountpoint, path)
    assert "ACQUIRE err=0" in out, out
    m = EXTENT_RE.search(out)
    assert m and int(m.group(2)) == 4096, f"expected the written 4 KiB first:\n{out}"
    assert int(m.group(3), 16) == 0, f"written extent with a flag:\n{out}"
    m1 = re.search(r"EXTENT 1 byte=\d+ bytes=\d+ flag=0x([0-9a-f]+)", out)
    assert m1 and int(m1.group(1), 16) & 0x01, f"unwritten extent without the flag:\n{out}"
    assert _raw(cijoe, dev_path, int(m.group(1))) == "DDDD", out

    cijoe.run(f"rm -f {path}")


def test_lease_refused_in_forked_child(cijoe):
    """A forked child cannot use the parent's leases, so it gets -ECHILD. Neither its close of the
    inherited descriptors at fork() nor xal_close() there drops the parent's lease, and
    xal_close() leaves alone the descriptors the child has opened since. xal_lease_stats() there
    does not wait for a lock held at fork()."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_fork"
    _fill(cijoe, path, "A", 4096)

    out = _capture(
        cijoe, f"sudo timeout -s KILL 20 xal_lease_hold {dev_path} {mountpoint} {path} --fork"
    )
    assert "CHILD_ACQUIRE err=-10" in out, out
    assert "CHILD_STATS err=0" in out, f"xal_lease_stats() failed in the child:\n{out}"
    assert "PARENT_LEASE=1" in out, f"the child's close released the parent's lease:\n{out}"
    assert "CHILD_FDS=64/64" in out, f"xal_close() in the child closed its descriptors:\n{out}"

    cijoe.run(f"rm -f {path}")


def test_lease_orphan_child_keeps_no_lease(cijoe):
    """A forked child does not keep the leases it inherits: after the parent exits without
    xal_close() while the child lives, a writer goes ahead at once instead of waiting
    lease-break-time for a lease nobody releases."""

    dev_path, mountpoint = _require_fixture(cijoe)
    path = f"{mountpoint}/xal_lease_orphan"
    _fill(cijoe, path, "A", 4096)

    cijoe.run("sudo sysctl -qw fs.lease-break-time=5")
    out = _capture(cijoe, f"sudo xal_lease_hold {dev_path} {mountpoint} {path} --orphan")
    m = re.search(r"ORPHAN child=(\d+)", out)
    assert m, out
    try:
        writer = _timed(
            cijoe, "lease_writer", f"printf B | dd of={path} conv=notrunc,fsync status=none"
        )
        took = int(_wait_for(writer, TOOK_RE, 15).group(1))
        assert took < 1000, f"writer took {took} ms after the parent exited"
        err, _ = cijoe.run(f"sudo kill -0 {m.group(1)}")
        assert not err, "the child was gone by the time the writer finished"
    finally:
        cijoe.run(f"sudo kill -KILL {m.group(1)}")

    cijoe.run(f"rm -f {path}")


def test_lease_rejects_shm(cijoe):
    """Leases pin nothing for other processes, so a shared index is refused."""

    dev_path, mountpoint = _require_fixture(cijoe)

    out = _capture(cijoe, f"sudo xal_lease_hold {dev_path} {mountpoint} {mountpoint} --shm")
    assert f"OPEN err=-{ENOTSUP}" in out, out
