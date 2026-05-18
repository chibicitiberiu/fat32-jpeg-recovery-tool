# How It Works

This document covers the techniques `sdrecov` uses to recover JPEGs from
damaged FAT32 media, and how `corrsim` exercises them.

## The problem

Three failure modes break naive carving:

1. **FAT damage.** Either the boot sector is gone (can't find the data area),
   or the FAT itself has bad entries (cluster chains terminate too early or
   point to wrong clusters).
2. **Fragmentation.** A file's clusters are not contiguous on disk. Without
   the FAT chain you can't tell which cluster comes next.
3. **In-file corruption.** A few bytes flipped inside the JPEG entropy data
   destroy decoding from that point onward, even if the file is otherwise
   intact.

Tools like `foremost` and PhotoRec handle (1) for contiguous files and
ignore (2) and (3). `sdrecov` addresses all three.

## Pipeline

```
┌──────────────────────────────────────────────────────────────────┐
│ 1. Geometry detection (boot sector + autodetect fallback)        │
│ 2. Dual-FAT merge (FAT1 + FAT2, single-bit correction)           │
│ 3. Cluster map (entropy + signature classification)              │
│ 4. Seed list (directory walk + SOI scan)                         │
│ 5. Template library (auto-build from intact headers)             │
│ 6. Per-seed recovery (parallel workers):                         │
│      a. Header parse (with template grafting fallback)           │
│      b. Fast-path: follow FAT chain, validate whole buffer       │
│      c. If fast-path rejects: DFS search with backtracking       │
│      d. Score candidates by FAT continuity, sequentiality,       │
│         Huffman validity, thumbnail similarity                   │
│      e. Write recovered file + optional RST-injected variant     │
└──────────────────────────────────────────────────────────────────┘
```

## Step 1: Geometry detection

The boot sector at offset 0 contains everything needed to interpret a
FAT32 filesystem: sector size, sectors per cluster, FAT location, data
area offset. If that sector is corrupt, FAT32 keeps a backup at sector 6 —
`sdrecov` tries that next.

If both are destroyed, `fat32_autodetect.cpp` scans for independent signals:

- **FAT signatures.** A FAT32 first sector contains the bytes
  `F8 FF FF 0F FF FF FF FF FF FF FF 0F` at offset 0 (the reserved entries).
  Scanning for this pattern locates FAT1 and FAT2; the gap between them is
  one FAT's worth of sectors.
- **Directory structure.** A root directory cluster starts with `.` and `..`
  entries or volume label entries with known field layouts.
- **JPEG SOI alignment.** Camera files tend to start at cluster boundaries.
  Counting SOI markers at each alignment hint reveals the cluster size.

These signals corroborate each other, so even single-source detection isn't
required. In practice this recovers geometry from cards with a completely
blank first MB.

## Step 2: Dual-FAT merge

FAT32 keeps two identical FATs as redundancy. If a cluster entry differs
between FAT1 and FAT2, the correct value is usually the one that's a valid
chain link (entry value `< 0x0FFFFFF8` and points to an allocated cluster).
For ambiguous cases, `fat_merge.cpp` enumerates single-bit-flip candidates
of both entries and picks the one whose neighbors form a coherent chain.

The merged FAT is the chain reference for fast-path recovery; mismatches are
recorded as "FAT-suspect" clusters that trigger extra validation downstream.

## Step 3: Cluster classification

`cluster_map.cpp` classifies every cluster on disk by content:

- **JPEG_HEADER** — starts with `FF D8 FF` (SOI + APP marker)
- **JPEG_ENTROPY** — high entropy, no FF-followed-by-non-stuffing
- **ZERO** — all bytes 0x00 (deleted or never-written)
- **TEXT** — printable ASCII proportion above threshold
- **OTHER** — anything else (probably not part of a JPEG)

Plus a refcount of how many FAT entries point at each cluster (>1 = cross-
linked, possibly damaged metadata).

This classification narrows the candidate set during search — a JPEG can
only continue into ENTROPY clusters, never into HEADER or TEXT.

## Step 4: Seed list

A "seed" is the starting cluster of a recoverable file. Two sources:

- **Directory entries.** Walking the FAT32 directory tree yields named files
  with their start clusters. This recovers original folder structure and
  filenames where the metadata is intact.
- **SOI signature scan.** For files whose directory entries are gone (or
  whose entries point at reused clusters), scan all JPEG_HEADER clusters and
  treat each as a seed. These get synthetic names like `seed_0042_unnamed.jpg`.

Seeds are deduplicated by start cluster — if both sources agree on the same
start, the named version wins.

## Step 5: Template library

Even when a file's own DHT (Huffman tables) and DQT (quantization tables)
are corrupted, you can often recover the file by grafting the header from
a sibling file shot on the same camera. `template_lib.cpp` scans the first
N intact JPEG headers and indexes them by DQT fingerprint. When
`header_graft.cpp` later encounters a file with an unparseable header, it
tries every template, prepending it to the file's entropy data and picking
the one that decodes the most MCUs.

The fallback below templates is ITU-T.81 Annex K standard tables — ~90% of
consumer cameras use these exact tables (Karresand & Shahmehri, 2008), so
they work as a last-resort substitution.

## Step 6: Per-seed recovery

This is the search problem. For each seed, we want the cluster chain that
maximizes JPEG correctness — bytes that decode without error, match the
EXIF thumbnail, and end at a clean EOI.

### Fast path

If the FAT chain rooted at the seed is intact, follow it directly. Read
all clusters into a single buffer, validate the entire concatenated
entropy stream once (using the Huffman validator), and if it passes, write
the file. This handles >90% of files on a lightly damaged card.

Fast-path also includes several repair heuristics for partial chain damage:

- **Chain truncation.** If validation fails mid-cluster, try truncating at
  the last valid cluster and writing what we have.
- **Sequential scan fallback.** If the chain terminates suspiciously early
  (chain bytes much less than `expected_size`), extend sequentially from the
  last cluster, scanning for an EOI marker — many cards have damaged FAT
  entries but contiguously allocated data.
- **Seam detection.** When the chain points to a cluster that's
  classified non-ENTROPY (zero, header, text), treat it as a seam and try
  the sequential continuation instead.
- **Cross-link detection.** A FAT entry jumping >100 clusters in an
  otherwise-sequential chain is usually corruption — treat the jump as a
  chain break.

### Huffman validation

The core decision oracle. `huffman.cpp` decodes the JPEG entropy bitstream
and looks for two failure signals:

1. **Invalid Huffman code lookup.** At every code length from 1 to 16, no
   code matches the next bits. JPEG Huffman tables have a "spare" code at
   each length (the last entry is reserved as invalid), and any decode that
   hits it is fragmentation.
2. **Quantization array overflow.** A decoded run-length symbol advances the
   zig-zag position past 64, which is impossible in a valid 8×8 block.

These two signals catch fragmentation boundaries with near-perfect accuracy
within the first filesystem block past the seam — the technique is from
van der Meer et al.'s DFRWS-EU 2024 paper. `huffman.cpp` adds checkpoint /
restore so the decoder can rewind to the last good state and try alternative
clusters.

### DFS with backtracking

When the fast path rejects (chain validation fails, or the seed has no
useful FAT chain), `engine.cpp` runs a depth-first search:

```
chain = [seed_cluster]
loop:
  candidates = neighbors near tail (FAT-suggested, sequential, nearby)
  for each candidate:
    if validate(chain + [candidate]) passes:
      score candidate; remember as branch point
  if any candidate scored:
    pick best, extend chain, continue
  else:
    backtrack to last branch point, try next-best
```

Scoring blends:

- **FAT continuity.** Strongest signal — if a candidate matches what the
  merged FAT says is next, it's almost always right.
- **Sequential proximity.** On SD cards, files are usually allocated in
  contiguous runs; preferring `cluster + 1` resolves most ties.
- **Huffman MCU count.** How many MCUs the candidate successfully decodes
  before validation fails.
- **Cluster classification fit.** ENTROPY clusters always beat ZERO or
  HEADER.

### Thumbnail validation

For seeds with an EXIF thumbnail, `thumbnail_validate.cpp` decodes the
thumbnail and the recovered main image and computes pixel similarity at
matched scale. A chain that produces a wrong-content image but happens to
pass Huffman validation will fail thumbnail similarity, which triggers
backtracking. This is especially useful on heavily fragmented cards where
multiple candidate chains all decode without errors but only one matches
the original photo.

### RST resync

Many cameras (Samsung phones, some Nikon DSLRs) insert restart markers
(FF D0–FF D7) every N MCUs as a recovery aid. If a chain has restart
markers and Huffman decoding fails mid-cluster, `rst_recovery.cpp` scans
forward to the next RST, resets the decoder state, and continues. The
resulting file has a visible glitch where the corrupted MCUs were, but
everything after the next RST decodes correctly.

For files without DRI markers (most camera JPEGs), `file_writer.cpp` can
inject a synthetic DRI marker and RST markers at gap boundaries, so the
post-gap data is still viewable in standard JPEG decoders. This is gated
by an MCU-count threshold so it only kicks in for severely partial recoveries.

## SOS bounds and other safety checks

The recovery loop never produces a file that would crash a downstream
decoder. Specifically, `jpeg_parse.cpp` validates:

- SOS Td/Ta table indices are within `[0, MAX_HUFF_TABLES)` — out-of-range
  values from corrupted SOS headers caused OOB on the real damaged card I
  built this for.
- DHT segments parse cleanly with non-empty symbol lists.
- DQT table IDs and precisions are sensible.

Files that fail these checks are rejected rather than written out as
"recovered."

## Feature flags

Each technique is wrapped in a `--enable-X` / `--disable-X` flag so the
ablation matrix is straightforward to run. The default is "everything on."
Disabling individual features is mainly useful for benchmark regression
testing — see `--help` for the full list.

## `corrsim`: the corruption simulator

`corrsim` exists because the only way to know recovery is correct is to
have ground truth. It builds a fresh FAT32 image, populates it with files
from a source directory, simulates user activity (deletes, moves, copies)
to fragment the allocation, and then applies one or more corruption passes:

- **bitflip** — per-bit error rate with degraded-block hotspots, biased
  0→1 (matches NAND wear)
- **ftl_block** — block-level damage (zeroed, swapped, wrong-data) simulating
  flash translation layer faults
- **metadata** — FAT chain breaks, directory entry corruption, FAT desync
  between FAT1 and FAT2
- **jpeg_targeted** — JPEG-specific attacks: DHT/DQT bit flips, SOF
  dimension corruption, byte-stuffing → accidental marker, RST mangling,
  partial-cluster zeroing, mid-cluster SOI shift, SOS table index OOB

A manifest is written alongside the image with each file's cluster chain,
size, and SHA-256, plus the SHA-256 of the original (pre-corruption) source.
This lets `tools/benchmark.py` score any recovery tool's output against the
truth: perfect/high/medium/low quality bins by pixel similarity, recovery
recall, false-positive count, and an F1 composite.

Profiles bundle parameter sets matching common failure modes — `light`,
`moderate`, `heavy` for general wear, `real-sd` calibrated to match the
30 GB card this project started from, and a handful of JPEG-targeted ones
that exercise specific recovery paths (`header-targeted`, `sos-corrupt`,
`fragmented`, etc.).
