# Fat32 JPEG Recovery Tool

A command-line recovery tool for damaged FAT32 SD cards. Combines FAT
chain reconstruction with JPEG Huffman stream validation to recover photos
from cards that boot sector damage, bit-rot, and cluster fragmentation have
made unreadable to normal mounting.

The repo contains two tools:

- **`sdrecov`** — the recovery engine. Takes a raw disk image and writes
  recovered JPEGs to an output directory.
- **`corrsim`** — a synthetic corruption simulator used for benchmarking.
  Builds a FAT32 image from a source folder of clean JPEGs, then applies
  configurable damage (bit flips, FAT corruption, cluster swaps, JPEG-targeted
  attacks).

## Why this exists

I had a 30 GB FAT32 SD card with a partly destroyed filesystem — family
photos from a few years' worth of weddings, trips, and birthdays. Existing
open-source carvers (PhotoRec, foremost, Scalpel) handle the easy case
(intact contiguous files) well but degrade quickly when the FAT is partly
gone, files are fragmented, or clusters within a file are damaged. They
recover bytes; they don't reason about whether the bytes form a valid JPEG.

`sdrecov` treats reconstruction as a search problem: at each cluster
boundary it asks "what are the candidate next clusters, and which one keeps
the Huffman bitstream valid?" — and uses FAT metadata, sequential
allocation priors, and EXIF thumbnail matching as scoring signals.

End result on the real card: 8,269 of an estimated 9,756 unique files
recovered intact, plus partial recovery of several hundred more.

## Build

Requires a C++17 compiler (g++ 9+ or clang 10+), `libjpeg-turbo` (for
EXIF thumbnail decoding), `make`, and `mtools`/`dosfstools` (only for
`corrsim`, to create FAT32 images).

```
# Debian / Ubuntu
sudo apt install build-essential libjpeg-turbo8-dev mtools dosfstools

# Fedora
sudo dnf install gcc-c++ make libjpeg-turbo-devel mtools dosfstools
```

Build both tools:

```
make -C recovery-tool
make -C corruption-sim
```

This produces `recovery-tool/sdrecov` and `corruption-sim/corrsim`.

## Usage

Image a damaged card first (use `ddrescue` if reads are slow or flaky):

```
ddrescue -d -r3 /dev/sdX card.img card.map
```

Recover:

```
sdrecov -i card.img -o recovered/ -p 1
```

Typical output:

```
sdrecov 0.1.0 starting
[stage 1/3] Loading disk image...
  Image: 0.1 GB (134217728 bytes)
  Partition offset: 0 bytes (sector 0)
  FAT32: 32700 clusters, 4096 bytes/cluster, 256 sectors/FAT
  FAT merged: 17369 valid, 15156 free, 171 eof, 4 corrupt, 0 bad
  Cross-linked clusters: 4
  FAT confidence: 100.0%
[stage 1/3] Classifying 32700 clusters...
  Clusters: 204 JPEG_HEADER, 17881 JPEG_SCAN, 395 NON_JPEG, 13720 EMPTY
[stage 1/3] Building seed list...
  Seeds: 185
[stage 1/3] Building template library...
  ... 15 templates from intact headers
[stage 2/3] Recovering ...
  seed 6787: extended premature-EOF chain 2014->3259 clusters
  seed 6787: suspect FAT chain (xlinks=1 fat_conf=100.0%), will try sequential
  recovered: files/0186_DSC_0043.JPG (13348885 bytes, RST-injected, 20 gaps)
  recovered: files/0187_DSC_0043.JPG (13348161 bytes, 3259 clusters, complete)
  ...
[stage 2/3] Done: 182 recovered, 1 failed, 0 header-fail, 2 skipped
```

Each recovered file ends up named `<seq>_<original or seed>.JPG`. The seq
prefix preserves ordering for browsers that sort alphabetically.

Common options:

```
-i, --image <file>          raw disk image
-o, --output <dir>          output directory
-p, --partition <N>         use MBR partition N (1-4); omit if no MBR
--partition-offset <bytes>  manual partition byte offset
--image2 <file>             second image of the same card for byte-level merge
--cluster-size <bytes>      override cluster size (when boot sector is gone)
--data-offset <bytes>       override data area offset
--threads <N>               worker threads (default 4)
--search greedy|beam|full   strategy (default greedy)
--beam-width <N>            chains kept per step in beam mode (default 3)
-v / -vv / -vvv             increasing verbosity
```

If both copies of the boot sector are destroyed, sdrecov can auto-detect
geometry by scanning for FAT signatures and directory entries — try the
default invocation first and only set `--cluster-size` / `--data-offset`
manually if it can't figure it out.

## Benchmarks

`corrsim` builds reproducible test images that span common SD-card failure
modes — bit-rot from NAND wear, FAT block damage, JPEG-specific header
attacks. Each scenario produces an image plus a manifest with cluster chains
and SHA-256s, which `tools/benchmark.py` uses to score recovery output.

Headline numbers from the standard 15-scenario suite (128 MB images, 100
files placed + simulator copies → ~155 active files per image, seed 42).
Counts are "perfect" — byte-exact match against ground truth.

| Scenario             | sdrecov | foremost |
|----------------------|--------:|---------:|
| none (clean baseline)|    155  |     153  |
| light                |    155  |     154  |
| heavy                |     55  |      54  |
| real-sd              |     95  |      94  |
| metadata-only        |    151  |     153  |
| bitrot-only          |    112  |     111  |
| fragmented           |    144  |     135  |
| header-targeted      |    155  |     153  |
| accidental-markers   |    155  |     153  |
| rst-targeted         |    155  |     153  |
| partial-zero         |    150  |     150  |
| seam-test            |    125  |     125  |
| deleted-only         |    155  |     153  |
| misaligned           |     79  |      78  |
| sos-corrupt          |    103  |     153  |

sdrecov leads or ties on 14 of 15 scenarios. The one loss is `sos-corrupt`:
that scenario plants out-of-range Huffman-table indices in the SOS header,
which sdrecov correctly rejects rather than risk an OOB read at decode time
(this was a real crash on the SD card that motivated the project). foremost
ignores the header structure and dumps raw bytes between SOI/EOI, so the
files it "recovers" happen to decode anyway in lenient viewers.

The biggest absolute wins are on `fragmented` (+9 perfect) and `none` /
`light` / `accidental-markers` / `rst-targeted` / `deleted-only` (+1-2,
sdrecov picks up files that foremost misses entirely because its EOI scan
trips on EXIF thumbnail markers). On heavily damaged scenarios (`heavy`,
`real-sd`) both tools recover only ~50-90 of the 600+ active fragments;
the remainder is genuinely unrecoverable.

Tradeoff: sdrecov runs more aggressively, so it produces more false
positives than foremost on most scenarios (e.g. 32 vs 24 on `light`, 109
vs 42 on `bitrot-only`). The extra files are typically severely partial
recoveries — viewable but degraded — rather than nonsense. For workflows
that prefer "fewer files, all good" over "more files, some partial,"
that's worth knowing.

To reproduce:

```
./tools/full_bench.sh          # full: 512MB / 400 files / ~30-60 min
./tools/full_bench.sh --quick  # 128MB / 100 files / ~10 min
```

The benchmark expects a source folder of clean JPEGs at `test-data/selected/`
to seed the simulator with. The repo does not include test images. A few
hundred camera JPEGs at mixed sizes (~200KB to ~10MB) work well; the more
varied the dimensions and quantization tables, the more useful the template
library becomes. If you don't have a personal photo set on hand, the
RAISE-1k dataset or any folder scraped from Wikimedia Commons category
"Photographs by camera" gives reasonable coverage.

## How it works

Quick version: `sdrecov` parses both copies of the FAT, merges them with
single-bit correction, classifies every cluster by content (JPEG header,
JPEG entropy, zeros, etc.), walks the directory tree plus signature-scans
for orphan JPEGs, then for each seed runs a search that picks the next
cluster by combining FAT continuity, sequential proximity, Huffman bitstream
validity, and EXIF thumbnail similarity into a single score.

For the techniques in detail — Huffman validation as a fragmentation
detector, standard-table fallback, header grafting from templates,
thumbnail-driven backtracking — see [HOW_IT_WORKS.md](HOW_IT_WORKS.md).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
