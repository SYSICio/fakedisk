# FDISK container format specification v1.0

fakedisk is a **single-file virtual disk container**: the entire directory tree and all file contents are stored inside one ordinary file. It is not a mountable disk image, but the companion tools (dos / windows) make it feel like a real drive.

## 1. Overall layout

The whole file is little-endian:

```
┌───────────────────────────────────────────┐
│ FDISK_HEADER (fixed, 96 bytes)             │
├───────────────────────────────────────────┤
│ directory table TOC (tree, serialized)     │
│   header.toc_offset .. +toc_size           │
├───────────────────────────────────────────┤
│ data area DATA (file contents, LZNT1)      │
│   header.data_offset .. +data_size         │
└───────────────────────────────────────────┘
```

- All multi-byte integers are little-endian and unsigned.
- File names are stored as **UTF-8**; tools convert to UTF-16 for display only.

## 2. FDISK_HEADER (96 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | magic | fixed bytes `46 44 49 53 4B 30 31 00` ("FDISK01\0") |
| 8 | 2 | version | format version, currently 1 |
| 10 | 2 | flags | bit0 = the container holds at least one compressed entry; all other bits reserved, write 0 |
| 12 | 32 | volume_label | volume label, UTF-8, zero-padded (up to 31 visible characters) |
| 44 | 8 | total_capacity | virtual capacity in bytes, used to report capacity and free space like a real drive |
| 52 | 8 | toc_offset | start of the directory table (96 for a new container) |
| 60 | 8 | toc_size | length of the directory table in bytes |
| 68 | 8 | data_offset | start of the data area (= toc_offset + toc_size) |
| 76 | 8 | data_size | length of the data area in bytes |
| 84 | 4 | toc_crc32 | CRC-32 of the directory table byte stream |
| 88 | 4 | header_crc32 | CRC-32 of bytes 8..87 (80 bytes) |
| 92 | 4 | reserved | reserved, write 0 |

## 3. Directory table TOC (tree)

The TOC is a recursively serialized directory tree. The **root node** sits at the start of the TOC; after it, each node's children are written recursively in the order "subdirectories first, then files".

### Node format

| Size | Field | Notes |
|---|---|---|
| 1 | type | 0 = file, 1 = directory |
| 1 | attr | attribute bits: bit0 = read-only, bit1 = hidden, bit2 = system, bit3 = archive |
| 2 | name_len | name length in bytes (UTF-8) |
| name_len | name | the name (must not contain `\`, `/` or NUL) |
| 8 | created | creation time, Unix seconds (0 = unknown) |
| 8 | modified | modification time, Unix seconds (0 = unknown) |
| 8 | file_size | **uncompressed** size of the file (0 for directories) |
| 8 | data_offset | offset of the content inside the data area (0 for directories) |
| 4 | stored_size | bytes the content occupies in the data area (0 for directories; equals file_size when uncompressed) |
| 1 | compressed | 0 = stored raw, 1 = LZNT1 compressed |
| 4 | crc32 | CRC-32 of the uncompressed content (0 for directories) |
| 4 | child_count | number of children (meaningful for directories only; files write 0) |
| recursive | children | child nodes, directories first, then files |

A file entry is at least 49 bytes excluding the name. Directories may nest to any depth.

## 4. Data area DATA

File contents are stored in the order their entries appear in the TOC; `data_offset` points at the start of each one.

- **Uncompressed**: raw bytes, `stored_size == file_size`.
- **Compressed**: Windows LZNT1 (`RtlCompressBuffer` / `RtlDecompressBuffer` from ntdll), `stored_size < file_size`.
- Deleting a file can leave a hole in the data area; `save` / `defrag` rebuild the data area so it stays compact.

## 5. Validation

- Opening a container checks, in order: magic, then header_crc32, then toc_crc32, then (optionally, with `fsck`) every file's content CRC.
- `fsck` decompresses every file, recomputes its CRC-32 and verifies that data offsets are in range and do not overlap.

## 6. Versioning and extension

- The `version` field allows future format upgrades; the header reserves 4 bytes at `reserved`.
- This specification defines only the minimal viable set: directory tree, attributes, timestamps, compression, CRC validation. Encryption, hard links and split volumes can be added later without disturbing existing fields.

## 7. Implementation conventions (v1.0 tools)

- `format <file> [size] [/v:label]`: creates a new container. Size accepts K/M suffixes; the default is 64M.
- Internal paths always use `/` as the separator (for example `/dir/sub/file.txt`); tools normalize both `/` and `\`.
- External files are imported through `copy` / `xcopy` / drag and drop and are compressed with LZNT1 automatically — the compressed form is used only when it is actually smaller.
- Opening an internal file: text, images and media are read and rendered in memory; executables are extracted to the system temporary directory (`%TEMP%\fakedisk_extract\<container name>\`) and then started.
