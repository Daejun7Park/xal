"""
Tests for how xal_open() treats the device it is given.

The default workflow leaves the filesystem unmounted.
"""

import os
from pathlib import Path

import pytest

EINVAL = 22


def _artifacts(cijoe) -> Path:
    p = Path(cijoe.getconf("xal.artifacts.path"))
    p.mkdir(parents=True, exist_ok=True)
    return p


def _capture(cijoe, cmd) -> str:
    """Run 'cmd', capturing stdout to an artifact file, and return it stripped."""

    out = _artifacts(cijoe) / "backend_cap.out"
    cijoe.run(f"{cmd} > {out} 2>/dev/null")
    return out.read_text().strip()


def _is_nvme(dev_path) -> bool:
    """Whether dev_path is an NVMe namespace, or a partition of one, which xNVMe identifies by
    ioctl whatever its name."""

    node = Path("/sys/class/block") / Path(os.path.realpath(dev_path)).name
    return (node / "nsid").exists() or (node.resolve().parent / "nsid").exists()


def test_backend_xfs_reports_read_error(cijoe):
    """When the superblock cannot be read, xal_open() returns that error, not errno. Through a
    symlink named unlike the device, xNVMe gets no geometry for a block device that is not NVMe,
    and dev_read() refuses the read with -EINVAL."""

    dev_path = cijoe.getconf("xal.dev_path", None)
    if _is_nvme(dev_path):
        pytest.skip("xNVMe identifies an NVMe namespace by ioctl, whatever its name")

    link = _artifacts(cijoe) / "xal_dev_link"
    cijoe.run(f"ln -sfn {dev_path} {link}")
    try:
        out = _capture(cijoe, f"xal --backend xfs --meta {link}")
    finally:
        cijoe.run(f"rm -f {link}")

    assert f"xal_open(...); err(-{EINVAL})" in out, out
