#!/usr/bin/env python3

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib
from pathlib import Path

DISK_SIZE = 64 * 1024 * 1024
SECTOR_SIZE = 512
DISK_SECTORS = DISK_SIZE // SECTOR_SIZE
# Each partition ends where the next starts: the root's ext2 (31 MiB), FAT16
# (16 MiB) from 32 MiB, ext3 (4 MiB) from 48 MiB and ext4 (8 MiB) from 52 MiB,
# which leaves the last 4 MiB free in both layouts. An MBR holds no more than
# these four.
ROOT_START = 2048
FAT16_START = 65536
EXT3_START = 98304
EXT4_START = 106496
EXT4_END = 122880
FAT16_CLUSTER_SECTORS = 4
FAT16_PARTITION_TYPE = 0x06
LINUX_PARTITION_TYPE = 0x83
PROTECTIVE_PARTITION_TYPE = 0xEE
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
# Partition entries take 32 sectors; with the header, each copy of the table
# needs 33.
GPT_TABLE_SECTORS = 1 + GPT_ENTRY_COUNT * GPT_ENTRY_SIZE // SECTOR_SIZE
BASIC_DATA_TYPE = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
LINUX_FILESYSTEM_TYPE = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
# Fixed GUIDs and times keep the image reproducible.
DISK_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D01")
FAT16_PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D02")
ROOT_PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D03")
EXT2_VOLUME_UUID = "6f2d7e52-3c8e-4d4b-9a51-7a0e5b1c2d04"
EXT3_VOLUME_UUID = "6f2d7e52-3c8e-4d4b-9a51-7a0e5b1c2d05"
EXT4_VOLUME_UUID = "6f2d7e52-3c8e-4d4b-9a51-7a0e5b1c2d06"
EXT3_PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D07")
EXT4_PARTITION_GUID = uuid.UUID("6F2D7E52-3C8E-4D4B-9A51-7A0E5B1C2D08")
EXT2_TIME = 1767225600
# mtools writes this file with a VFAT long name, which the kernel must read.
LONG_NAME_FILE = "Read Me First.txt"
LONG_NAME_CONTENTS = b"Long names from mtools work.\n"
# The root's directories besides lost+found: the shell starts in /home, and
# PATH ends with /bin.
ROOT_DIRECTORIES = ("home", "bin")
# Files under /samples, which the STORAGE kernel test and the shell smoke test
# read. big.txt is past 64 KiB, so it needs an indirect block.
SAMPLES = "samples"
EXT2_FILES = {
    "hello.txt": b"Hello from the ext2 partition.\n",
    "big.txt": b"".join(b"ext2 line %05d\n" % line for line in range(5000)),
    "docs/notes/deep.txt": b"nested ext2 file\n",
    "a-file-name-longer-than-sixty-three-characters-that-fat16-cannot-hold"
    ".txt": b"long ext2 name\n",
}
# Under 60 bytes a link's target lives in its inode; a longer one takes a
# block of its own.
EXT2_LINKS = {
    "fast-link": "hello.txt",
    "slow-link": "docs/notes/" + "../notes/" * 5 + "deep.txt",
}
# Owners and modes, which `ls -l` in the shell test shows: whatever builds
# the image, /samples and everything in it belong to user and group 1000, the
# rest to 0, files are 0644, directories 0755 and links 0777, and hello.txt is
# 0640, owned by group 100.
EXT2_OWNER = 1000
EXT2_SPECIAL_MODES = {"hello.txt": 0o640}
EXT2_SPECIAL_GROUPS = {"hello.txt": 100}
# The ext3 and ext4 partitions, which FeatherOS mounts read-only and must not
# write: the STORAGE kernel test and the shell part ext4.py read them, and
# everything on them belongs to user and group 0. big.txt needs an indirect
# block; extents.bin is 2 MiB of 1 KiB blocks in which every fourth block is
# zeros, which mke2fs leaves out as a hole, so its 512 extents need an extent
# tree of depth 2.
EXT3_FILES = {
    "hello.txt": b"Hello from the ext3 partition.\n",
    "big.txt": b"".join(b"ext3 line %05d\n" % line for line in range(3000)),
    "docs/readme.txt": b"ext3 subdirectory file\n",
}
EXT3_LINKS = {"link": "docs/readme.txt"}
EXT4_BLOCK_SIZE = 1024
EXT4_EXTENT_BLOCKS = 2048
EXT4_HOLE_EVERY = 4


def ext4_block(block):
    if block % EXT4_HOLE_EVERY == EXT4_HOLE_EVERY - 1:
        return bytes(EXT4_BLOCK_SIZE)
    return b"".join(
        b"ext4 block %04d line %02d extents\n" % (block, line)
        for line in range(EXT4_BLOCK_SIZE // 32)
    )


EXT4_FILES = {
    "hello.txt": b"Hello from the ext4 partition.\n",
    "extents.bin": b"".join(
        ext4_block(block) for block in range(EXT4_EXTENT_BLOCKS)
    ),
    "docs/nested/deep.txt": b"nested ext4 file\n",
}
EXT4_LINKS = {"link": "docs/nested/deep.txt"}
E2FSPROGS_DIRECTORIES = (
    "/opt/homebrew/opt/e2fsprogs/sbin",
    "/usr/local/opt/e2fsprogs/sbin",
)


def mbr_entry(partition_type, first_sector, sector_count):
    entry = bytearray(16)
    entry[1:4] = bytes((0, 2, 0))
    entry[4] = partition_type
    entry[5:8] = bytes((0xFE, 0xFF, 0xFF))
    struct.pack_into("<II", entry, 8, first_sector, sector_count)
    return entry


def write_mbr(disk_file, *entries):
    mbr = bytearray(SECTOR_SIZE)
    for index, entry in enumerate(entries):
        mbr[446 + 16 * index : 462 + 16 * index] = entry
    mbr[510:512] = b"\x55\xaa"
    disk_file.seek(0)
    disk_file.write(mbr)


def gpt_header(current, backup, entries_sector, entries_crc):
    header = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        b"EFI PART",
        0x00010000,
        92,
        0,
        0,
        current,
        backup,
        GPT_TABLE_SECTORS + 1,
        DISK_SECTORS - GPT_TABLE_SECTORS - 1,
        DISK_GUID.bytes_le,
        entries_sector,
        GPT_ENTRY_COUNT,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    # The header CRC covers the header with its own CRC field zeroed.
    struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
    return header


def write_gpt(disk_file, partitions):
    entries = bytearray(GPT_ENTRY_COUNT * GPT_ENTRY_SIZE)
    for index, (type_guid, guid, first, last, name) in enumerate(partitions):
        struct.pack_into(
            "<16s16sQQQ72s",
            entries,
            index * GPT_ENTRY_SIZE,
            type_guid.bytes_le,
            guid.bytes_le,
            first,
            last,
            0,
            name.encode("utf-16-le"),
        )
    entries_crc = zlib.crc32(entries)
    backup_header = DISK_SECTORS - 1
    backup_entries = DISK_SECTORS - GPT_TABLE_SECTORS
    write_mbr(
        disk_file, mbr_entry(PROTECTIVE_PARTITION_TYPE, 1, DISK_SECTORS - 1)
    )
    for header_sector, entries_sector, other in (
        (1, 2, backup_header),
        (backup_header, backup_entries, 1),
    ):
        disk_file.seek(entries_sector * SECTOR_SIZE)
        disk_file.write(entries)
        disk_file.seek(header_sector * SECTOR_SIZE)
        disk_file.write(
            gpt_header(header_sector, other, entries_sector, entries_crc)
        )


def find_mke2fs():
    """Homebrew's keg-only e2fsprogs first, as src/fs/core's CMake looks."""
    for directory in E2FSPROGS_DIRECTORIES:
        candidate = Path(directory) / "mke2fs"
        if os.access(candidate, os.X_OK):
            return str(candidate)
    found = shutil.which("mke2fs") or shutil.which(
        "mke2fs", path="/usr/sbin:/sbin"
    )
    if found is None:
        sys.exit(
            "create_test_disk.py: mke2fs not found; install e2fsprogs "
            "(brew install e2fsprogs, or apt install e2fsprogs)"
        )
    return found


def find_debugfs(mke2fs):
    beside = Path(mke2fs).parent / "debugfs"
    if os.access(beside, os.X_OK):
        return str(beside)
    found = shutil.which("debugfs") or shutil.which(
        "debugfs", path="/usr/sbin:/sbin"
    )
    if found is None:
        sys.exit("create_test_disk.py: debugfs not found beside mke2fs")
    return found


def root_owner_of(name):
    if name != SAMPLES and not name.startswith(SAMPLES + "/"):
        return 0, 0
    inner = name[len(SAMPLES) + 1 :]
    return EXT2_OWNER, EXT2_SPECIAL_GROUPS.get(inner, EXT2_OWNER)


def set_owners(debugfs, volume_path, names, links, owner_of, environment):
    """-d copies the host's owners, and the host's modes for links."""
    commands = []
    for name in names:
        user, group = owner_of(name)
        commands.append(f"sif /{name} uid {user}")
        commands.append(f"sif /{name} gid {group}")
        if name in links:
            commands.append(f"sif /{name} mode 0120777")
    with tempfile.NamedTemporaryFile("w", suffix=".txt") as command_file:
        command_file.write("\n".join(commands) + "\n")
        command_file.flush()
        result = subprocess.run(
            [debugfs, "-w", "-f", command_file.name, str(volume_path)],
            capture_output=True,
            text=True,
            env=environment,
            check=False,
        )
    # debugfs echoes each command after "debugfs:" and exits 0 even when one
    # fails, so any other line is an error.
    errors = [
        line
        for line in (result.stdout + result.stderr).splitlines()
        if line and not line.startswith("debugfs")
    ]
    if result.returncode != 0 or errors:
        sys.exit("create_test_disk.py: debugfs failed:\n" + "\n".join(errors))


def write_files(directory, files, links, special_modes=None):
    for name, contents in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        path.chmod((special_modes or {}).get(name, 0o644))
    for name, target in links.items():
        (directory / name).symlink_to(target)


def finish_tree(source):
    """Reproducible directory modes and times; the names, sorted."""
    source.chmod(0o755)
    for path in source.rglob("*"):
        if path.is_dir() and not path.is_symlink():
            path.chmod(0o755)
    for path in sorted(source.rglob("*"), reverse=True):
        os.utime(path, (EXT2_TIME, EXT2_TIME), follow_symlinks=False)
    return sorted(str(path.relative_to(source)) for path in source.rglob("*"))


def make_root_tree(source, block_file):
    """The root's staging directory."""
    (source / "BLOCK.TXT").write_bytes(Path(block_file).read_bytes())
    (source / "BLOCK.TXT").chmod(0o644)
    for directory in ROOT_DIRECTORIES:
        (source / directory).mkdir()
    write_files(source / SAMPLES, EXT2_FILES, EXT2_LINKS, EXT2_SPECIAL_MODES)
    return finish_tree(source)


def run_mke2fs(
    mke2fs,
    options,
    label,
    volume_uuid,
    source,
    volume_path,
    sector_count,
    environment,
):
    # mke2fs warns about 128-byte inodes even with -q, so its output is shown
    # only when it fails.
    result = subprocess.run(
        [
            mke2fs,
            "-q",
            "-F",
            *options,
            "-L",
            label,
            "-U",
            volume_uuid,
            "-E",
            f"hash_seed={volume_uuid},root_owner=0:0",
            "-d",
            str(source),
            str(volume_path),
            str(sector_count * SECTOR_SIZE // 1024),
        ],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        sys.exit(
            f"create_test_disk.py: mke2fs failed:\n{result.stdout}"
            f"{result.stderr}"
        )


def make_ext2_volume(volume_path, sector_count, block_file):
    environment = dict(os.environ, E2FSPROGS_FAKE_TIME=str(EXT2_TIME))
    mke2fs = find_mke2fs()
    with tempfile.TemporaryDirectory() as source_name:
        source = Path(source_name)
        names = make_root_tree(source, block_file)
        run_mke2fs(
            mke2fs,
            ("-t", "ext2", "-b", "1024", "-I", "128"),
            "FEATHEROS",
            EXT2_VOLUME_UUID,
            source,
            volume_path,
            sector_count,
            environment,
        )
    links = {f"{SAMPLES}/{name}" for name in EXT2_LINKS}
    set_owners(
        find_debugfs(mke2fs),
        volume_path,
        names,
        links,
        root_owner_of,
        environment,
    )


def make_journalled_volume(volume_path, sector_count, kind):
    """ext3, or ext4 with mke2fs's default features (extents, 64bit, flex_bg,
    metadata_csum, ...); 1 KiB blocks and 256-byte inodes whatever the host's
    mke2fs.conf says for small volumes."""
    files, links, label, volume_uuid = {
        "ext3": (EXT3_FILES, EXT3_LINKS, "FEATHEREXT3", EXT3_VOLUME_UUID),
        "ext4": (EXT4_FILES, EXT4_LINKS, "FEATHEREXT4", EXT4_VOLUME_UUID),
    }[kind]
    environment = dict(os.environ, E2FSPROGS_FAKE_TIME=str(EXT2_TIME))
    mke2fs = find_mke2fs()
    with tempfile.TemporaryDirectory() as source_name:
        source = Path(source_name)
        write_files(source, files, links)
        names = finish_tree(source)
        run_mke2fs(
            mke2fs,
            ("-t", kind, "-b", "1024", "-I", "256"),
            label,
            volume_uuid,
            source,
            volume_path,
            sector_count,
            environment,
        )
    set_owners(
        find_debugfs(mke2fs),
        volume_path,
        names,
        set(links),
        lambda name: (0, 0),
        environment,
    )


def make_fat16_volume(image, fat16_sectors):
    image_spec = f"{image}@@{FAT16_START * SECTOR_SIZE}"
    subprocess.run(
        [
            "mformat",
            "-i",
            image_spec,
            "-H",
            str(FAT16_START),
            "-T",
            str(fat16_sectors),
            # Left to choose, mformat may make FAT12 of a partition this small.
            "-c",
            str(FAT16_CLUSTER_SECTORS),
            "-v",
            "FEATHERFAT",
            "::",
        ],
        check=True,
    )
    # The STORAGE test's shell session works in /home there.
    subprocess.run(["mmd", "-i", image_spec, "::home"], check=True)
    with tempfile.NamedTemporaryFile() as long_name_source:
        long_name_source.write(LONG_NAME_CONTENTS)
        long_name_source.flush()
        subprocess.run(
            [
                "mcopy",
                "-i",
                image_spec,
                long_name_source.name,
                f"::{LONG_NAME_FILE}",
            ],
            check=True,
        )


def copy_volume(volume, disk_path, start):
    with volume.open("rb") as volume_file, disk_path.open("r+b") as disk_file:
        disk_file.seek(start * SECTOR_SIZE)
        shutil.copyfileobj(volume_file, disk_file)


def main():
    parser = argparse.ArgumentParser(
        description="Create a small MBR or GPT disk with the root's ext2 "
        "partition, then FAT16, ext3 and ext4 partitions."
    )
    parser.add_argument("image", help="Output raw disk image")
    parser.add_argument(
        "file", help="File to copy into the root directory as BLOCK.TXT"
    )
    parser.add_argument(
        "--layout",
        choices=("mbr", "gpt"),
        default="mbr",
        help="Partition table type",
    )
    args = parser.parse_args()

    # GPT's backup table in the disk's last sectors stays clear of the free
    # space after ext4.
    assert EXT4_END <= DISK_SECTORS - GPT_TABLE_SECTORS - 1
    root_sectors = FAT16_START - ROOT_START
    fat16_sectors = EXT3_START - FAT16_START
    ext3_sectors = EXT4_START - EXT3_START
    ext4_sectors = EXT4_END - EXT4_START

    image = Path(args.image).resolve()
    image.parent.mkdir(parents=True, exist_ok=True)
    with image.open("wb") as disk_file:
        disk_file.truncate(DISK_SIZE)

    with tempfile.TemporaryDirectory() as work:
        volume = Path(work) / "root.img"
        make_ext2_volume(volume, root_sectors, args.file)
        copy_volume(volume, image, ROOT_START)
        for kind, start, sectors in (
            ("ext3", EXT3_START, ext3_sectors),
            ("ext4", EXT4_START, ext4_sectors),
        ):
            volume = Path(work) / f"{kind}.img"
            make_journalled_volume(volume, sectors, kind)
            copy_volume(volume, image, start)
    make_fat16_volume(image, fat16_sectors)

    with image.open("r+b") as disk_file:
        if args.layout == "gpt":
            write_gpt(
                disk_file,
                (
                    (
                        LINUX_FILESYSTEM_TYPE,
                        ROOT_PARTITION_GUID,
                        ROOT_START,
                        FAT16_START - 1,
                        "FEATHEROS",
                    ),
                    (
                        BASIC_DATA_TYPE,
                        FAT16_PARTITION_GUID,
                        FAT16_START,
                        EXT3_START - 1,
                        "FEATHERFAT",
                    ),
                    (
                        LINUX_FILESYSTEM_TYPE,
                        EXT3_PARTITION_GUID,
                        EXT3_START,
                        EXT4_START - 1,
                        "FEATHEREXT3",
                    ),
                    (
                        LINUX_FILESYSTEM_TYPE,
                        EXT4_PARTITION_GUID,
                        EXT4_START,
                        EXT4_END - 1,
                        "FEATHEREXT4",
                    ),
                ),
            )
        else:
            write_mbr(
                disk_file,
                mbr_entry(LINUX_PARTITION_TYPE, ROOT_START, root_sectors),
                mbr_entry(FAT16_PARTITION_TYPE, FAT16_START, fat16_sectors),
                mbr_entry(LINUX_PARTITION_TYPE, EXT3_START, ext3_sectors),
                mbr_entry(LINUX_PARTITION_TYPE, EXT4_START, ext4_sectors),
            )


if __name__ == "__main__":
    main()
