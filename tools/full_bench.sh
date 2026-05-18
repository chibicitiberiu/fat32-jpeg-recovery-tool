#!/bin/bash
#
# full_bench.sh - Complete benchmark: generate scenarios, run all tools, evaluate
#
# Usage: ./tools/full_bench.sh [--quick]
#   --quick: 128MB/100 files/single seed (for fast iteration)
#   default: 512MB/400 files/single seed 42
#
# Runtime: ~30-60 minutes for full, ~10 minutes for quick
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

QUICK=false
[[ "$1" == "--quick" ]] && QUICK=true

CORRSIM="corruption-sim/corrsim"
SDRECOV="recovery-tool/sdrecov"
SOURCE_DIR="test-data/selected"
SCENARIOS_DIR="test-data/scenarios"
RESULTS_DIR="benchmark_results"

# Build tools first
echo "=== Building tools ==="
(cd corruption-sim && make -j$(nproc)) 2>&1 | tail -1
(cd recovery-tool && make -j$(nproc)) 2>&1 | tail -1
echo ""

# Config
if $QUICK; then
    FILE_COUNT=100; IMAGE_SIZE=128; SIM_OPS=100
    echo "Quick mode: ${IMAGE_SIZE}MB, ${FILE_COUNT} files"
else
    FILE_COUNT=400; IMAGE_SIZE=512; SIM_OPS=200
    echo "Full mode: ${IMAGE_SIZE}MB, ${FILE_COUNT} files"
fi
SEED=42

SCENARIOS="none metadata-only light moderate heavy real-sd header-targeted accidental-markers rst-targeted partial-zero bitrot-only fragmented seam-test deleted-only misaligned sos-corrupt"

# Step 1: Generate scenarios
echo ""
echo "============================================"
echo "  STEP 1: Generate scenario images"
echo "============================================"
echo ""

rm -rf "$SCENARIOS_DIR"
mkdir -p "$SCENARIOS_DIR"

for scenario in $SCENARIOS; do
    img="$SCENARIOS_DIR/${scenario}.img"
    manifest="$SCENARIOS_DIR/${scenario}_truth.json"

    pass_args=""
    if [ "$scenario" = "none" ]; then
        pass_args="--passes 0"
    else
        pass_args="--profile $scenario"
    fi

    echo -n "  $scenario: "
    "$CORRSIM" -s "$SOURCE_DIR" -o "$img" \
        --size $IMAGE_SIZE --file-count $FILE_COUNT --seed $SEED --sim-ops $SIM_OPS \
        $pass_args \
        --manifest "$manifest" -v 2>&1 | tail -1
done

echo ""
echo "Generated $(ls "$SCENARIOS_DIR"/*.img 2>/dev/null | wc -l) images"

# Step 2: Run all tools
echo ""
echo "============================================"
echo "  STEP 2: Run recovery tools"
echo "============================================"
echo ""

rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

for scenario in $SCENARIOS; do
    img="$SCENARIOS_DIR/${scenario}.img"
    manifest="$SCENARIOS_DIR/${scenario}_truth.json"
    [ ! -f "$manifest" ] && continue

    geo=$(python3 -c "import json; m=json.load(open('$manifest')); g=m['geometry']; print(g['bytes_per_cluster'], g['data_offset'])")
    bpc=$(echo $geo | awk '{print $1}')
    doff=$(echo $geo | awk '{print $2}')

    echo "--- $scenario ---"

    # sdrecov: greedy, beam, full
    for mode in greedy beam full; do
        outdir="$RESULTS_DIR/${scenario}_sdrecov-${mode}"
        echo -n "  sdrecov-${mode}: "
        t0=$(date +%s)
        timeout 1800 "$SDRECOV" \
            -i "$img" --partition-offset 0 --cluster-size "$bpc" --data-offset "$doff" \
            -o "$outdir" --search "$mode" --beam-width 3 -v 2>"$outdir.log" || true
        t1=$(date +%s)
        nfiles=$(find "$outdir/files" -type f 2>/dev/null | wc -l)
        echo "$nfiles files ($((t1-t0))s)"
    done

    # foremost
    if command -v foremost &>/dev/null; then
        outdir="$RESULTS_DIR/${scenario}_foremost"
        echo -n "  foremost: "
        t0=$(date +%s)
        rm -rf "${outdir}_tmp" "$outdir"
        mkdir -p "$outdir/files"
        timeout 300 foremost -t jpg -i "$img" -o "${outdir}_tmp" 2>/dev/null || true
        find "${outdir}_tmp" -name "*.jpg" -exec cp {} "$outdir/files/" \; 2>/dev/null || true
        rm -rf "${outdir}_tmp"
        t1=$(date +%s)
        nfiles=$(find "$outdir/files" -type f 2>/dev/null | wc -l)
        echo "$nfiles files ($((t1-t0))s)"
    fi

    # photorec - disabled: batch mode doesn't write files when stdin is /dev/null
    # Run manually if needed: photorec /d <outdir> /cmd <img> partition_none,...
    if false && command -v photorec &>/dev/null; then
        outdir="$RESULTS_DIR/${scenario}_photorec"
        echo -n "  photorec: "
        t0=$(date +%s)
        rm -rf "$outdir"
        mkdir -p "$outdir/files"
        timeout --signal=KILL 120 photorec /d "$outdir/files" /cmd "$img" partition_none,options,mode_ext2,fileopt,everything,disable,jpg,enable,search </dev/null >/dev/null 2>&1 || true
        find "$outdir" -name "*.jpg" -not -path "$outdir/files/*" -exec mv {} "$outdir/files/" \; 2>/dev/null || true
        t1=$(date +%s)
        nfiles=$(find "$outdir/files" -type f 2>/dev/null | wc -l)
        echo "$nfiles files ($((t1-t0))s)"
    fi

    echo ""
done

# Step 3: Evaluate all results
echo "============================================"
echo "  STEP 3: Evaluate (composite scoring)"
echo "============================================"
echo ""

python3 << 'PYEOF'
import sys, os
sys.path.insert(0, 'tools')
from benchmark import load_manifest, compare_results

source_dir = 'test-data/selected'
results_dir = 'benchmark_results'
scenarios_dir = 'test-data/scenarios'

scenarios = [f.replace('_truth.json','') for f in sorted(os.listdir(scenarios_dir)) if f.endswith('_truth.json')]

header = f"{'Scenario':<22} {'Tool':<18} {'Perf':>5} {'High':>5} {'Med':>4} {'Low':>4} {'Miss':>5} {'FP':>4} {'BinSim':>6} {'Comp':>5} {'Rec%':>5} {'Prec':>5} {'F1':>5}"
print(header)
print("-" * len(header))
sys.stdout.flush()

for scenario in scenarios:
    manifest = os.path.join(scenarios_dir, f'{scenario}_truth.json')
    try:
        m = load_manifest(manifest)
    except:
        continue

    tools = []
    for d in sorted(os.listdir(results_dir)):
        if d.startswith(scenario + '_') and os.path.isdir(os.path.join(results_dir, d)):
            tool = d[len(scenario)+1:]
            tools.append((tool, os.path.join(results_dir, d)))

    for tool, outdir in tools:
        sys.stderr.write(f"  evaluating {scenario}/{tool}...\n")
        sys.stderr.flush()
        try:
            r = compare_results(outdir, m, source_dir)
            print(f"{scenario:<22} {tool:<18} {r['perfect']:>5} {r['high_quality']:>5} {r['medium_quality']:>4} {r['low_quality']:>4} {r['missed']:>5} {r['false_positives']:>4} {r['avg_similarity']:>6.3f} {r['avg_composite']:>5.3f} {r['d_recovery']*100:>5.1f} {r['d_precision']:>5.3f} {r['d_f1']:>5.3f}")
            sys.stdout.flush()
        except Exception as e:
            print(f"{scenario:<22} {tool:<18} ERROR: {e}")
            sys.stdout.flush()

    if tools:
        print()
        sys.stdout.flush()
PYEOF

echo ""
echo "============================================"
echo "  DONE"
echo "============================================"
