# xv6-raid

A software RAID layer implemented as an extension to the xv6 (RISC-V) teaching
operating system, developed as a project for the Operating Systems 2 course.
Implements RAID0, RAID1, RAID0+1, RAID4, and RAID5 across multiple disks,
exposed to user programs as a set of system calls.

> **Note:** This repository contains the RAID subsystem source added on top
> of a course-modified xv6-riscv base. xv6 itself is open-source under the
> MIT License (see [mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv)).
> The specific course-provided base (disk emulation support, build
> configuration) is not redistributed here — see **Building** below for how
> to reconstruct a working environment.

## Design Highlights

- Supports RAID0, RAID1, RAID0+1, RAID4, and RAID5 across up to 7 disks
  (an 8th disk remains reserved for xv6's own file system)
- Seven system calls covering initialization, block-level read/write, disk
  failure/repair, structure info, and teardown
- RAID metadata is written to disk on initialization and persists across
  system reboots
- Concurrent access from multiple processes is synchronized via per-disk
  locks acquired in ascending index order, preventing deadlock; a
  sleep/wakeup-based busy-state check serializes access to each disk
- RAID4/5 support parity-based data reconstruction after a disk failure and
  repair

## Features

- [x] RAID0, RAID1, RAID0+1 (striping, mirroring, and combined)
- [x] RAID4, RAID5 (block-level parity)
- [x] Live disk fail/repair with data reconstruction
- [x] Thread-/process-safe concurrent access

## System Call Interface

| Signature | Description |
|---|---|
| `int init_raid(enum RAID_TYPE raid)` | Initializes the RAID structure with the given variant and persists its metadata to disk. |
| `int read_raid(int blkn, uchar* data)` | Reads one logical block into `data`. |
| `int write_raid(int blkn, uchar* data)` | Writes one logical block from `data`. |
| `int disk_fail_raid(int diskn)` | Marks a disk as failed; the array continues operating in degraded mode where the RAID variant allows it. |
| `int disk_repaired_raid(int diskn)` | Marks a previously failed disk as repaired and triggers reconstruction of its data. |
| `int info_raid(uint* blkn, uint* blks, uint* diskn)` | Reports usable logical block count, block size, and disk count. |
| `int destroy_raid()` | Tears down the RAID structure; the disks are no longer usable as a RAID array afterward. |

All calls return `0` on success and a negative value on error.

## Requirements

- A RISC-V toolchain (e.g. `riscv64-unknown-elf-gcc` or `riscv64-linux-gnu-gcc`)
- `qemu-system-riscv64`
- The course-provided, modified xv6-riscv base (adds multi-disk support and
  the `write_block`/`read_block` interface this project builds on)

> **Note:** Vanilla xv6-riscv can be built on any Linux/macOS/WSL system with
> the toolchain above — it does not require a special VM. This specific
> project, however, depends on the course's modified base and its Makefile
> disk configuration, which are not included in this repository.

## Building & Running

1. Obtain the course-modified xv6-riscv base (from the course website or
   instructor) and set up the toolchain and QEMU as described above.
2. Clone this repository and copy the source files into the corresponding
   folders of the xv6 base.
3. Configure the number and size of RAID disks in the `Makefile`
   (`RAID_DISKS`/equivalent — see course base documentation).
4. Run `make qemu` to build and boot the system in QEMU.
5. Use the provided user-space test programs (or your own, via the
   `init_raid`/`read_raid`/`write_raid`/... syscalls) to exercise the RAID
   layer.

