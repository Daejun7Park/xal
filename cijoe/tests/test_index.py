import logging as log
from pprint import pprint
from pathlib import Path


def test_compare_to_find(cijoe):

    dev_path = cijoe.getconf("xal.dev_path", None)
    mountpoint = cijoe.getconf("xal.mountpoint", None)

    paths = {
        "find": Path(cijoe.getconf("xal.artifacts.path")) / "find.output",
        "xal": Path(cijoe.getconf("xal.artifacts.path")) / "xal_find.output",
    }
    indexes = {
        "find": [],
        "xal": [],
    }

    # Have 'xal' produce the 'find-like' index
    err, state = cijoe.run(f"xal --find {dev_path} > {paths['xal']}")
    assert not err

    for key, path in paths.items():
        for line in sorted(paths[key].read_text().splitlines()):
            indexes[key].append(
                line.replace(dev_path if key == "xal" else mountpoint, "")
            )

    diffs = []
    for expected, got in zip(indexes["find"], indexes["xal"]):
        if expected == got:
            continue

        diffs.append({"expected": expected, "got": got})

    assert not diffs


READ_AHEAD_FILL = """
import os, sys

# A directory of four entries with 100-byte names no longer fits in its inode: one block each
for d in range(300):
    path = os.path.join(sys.argv[1], f"d{d:03d}")
    os.mkdir(path)
    for f in range(4):
        open(os.path.join(path, f"{f}" + "x" * 99), "w").close()
"""


def test_compare_to_find_past_the_read_ahead_limit(cijoe):
    """On a filesystem with more directory blocks than the read-ahead holds, which is as many as
    the allocated inodes fill, the read-ahead stops at its limit and the walk reads the rest: the
    index still matches find."""

    art = Path(cijoe.getconf("xal.artifacts.path"))
    img, mnt, loop = art / "readahead.img", art / "readahead.mnt", art / "readahead.loop"
    fill, found, indexed = art / "readahead_fill.py", art / "readahead.find", art / "readahead.xal"

    fill.write_text(READ_AHEAD_FILL)
    err, _ = cijoe.run(f"rm -f {img} && truncate -s 512M {img} && mkdir -p {mnt}")
    assert not err
    err, _ = cijoe.run(f"sudo losetup -f --show {img} > {loop}")
    assert not err
    dev = loop.read_text().strip()
    try:
        err, _ = cijoe.run(
            f"sudo mkfs.xfs -f -q {dev} && sudo mount {dev} {mnt} && "
            f"sudo python3 {fill} {mnt} && find {mnt} | sort > {found} && sudo umount {mnt}"
        )
        assert not err
        err, _ = cijoe.run(f"sudo xal --find {dev} > {indexed}")
        assert not err

        expected = sorted(line.replace(str(mnt), "") for line in found.read_text().splitlines())
        got = sorted(line.replace(dev, "") for line in indexed.read_text().splitlines())
        assert len(expected) > 1500
        assert got == expected
    finally:
        cijoe.run(f"sudo umount {mnt}; sudo losetup -d {dev}; rm -f {img}")
