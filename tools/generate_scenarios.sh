#!/bin/bash
#
# generate_scenarios.sh - Create test disk images for benchmarking
#
# Creates larger images with more files and additional JPEG-targeted
# corruption scenarios. Runs multiple seeds for statistical confidence.
#
# Usage: ./tools/generate_scenarios.sh [--quick]
#   --quick: 128MB/100 files/single seed (for fast iteration)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

CORRSIM="$ROOT_DIR/corruption-sim/corrsim"
SOURCE_DIR="$ROOT_DIR/test-data/selected"
SCENARIOS_DIR="$ROOT_DIR/test-data/scenarios"

# Default: larger images for thorough testing
FILE_COUNT=400
IMAGE_SIZE=512
SIM_OPS=200
SEEDS="42 123 777"

# Quick mode for fast iteration
if [[ "$1" == "--quick" ]]; then
    FILE_COUNT=100
    IMAGE_SIZE=128
    SIM_OPS=100
    SEEDS="42"
    echo "Quick mode: ${IMAGE_SIZE}MB, ${FILE_COUNT} files, seed 42 only"
fi

if [ ! -f "$CORRSIM" ]; then
    echo "error: corrsim not found at $CORRSIM"
    echo "Run 'make' in corruption-sim/ first."
    exit 1
fi

rm -rf "$SCENARIOS_DIR"
mkdir -p "$SCENARIOS_DIR"

echo "=== Generating benchmark scenarios ==="
echo "Source: $SOURCE_DIR ($(ls "$SOURCE_DIR" | wc -l) files)"
echo "Output: $SCENARIOS_DIR/"
echo "Config: ${IMAGE_SIZE}MB, ${FILE_COUNT} files, seeds: ${SEEDS}"
echo ""

# Core scenarios (run with all seeds)
CORE_SCENARIOS="none metadata-only light moderate heavy real-sd"

# Targeted scenarios (JPEG-specific, single seed for now)
TARGETED_SCENARIOS="fragmented bitrot-only header-targeted accidental-markers rst-targeted partial-zero seam-test deleted-only misaligned sos-corrupt"

generate_scenario() {
    local profile=$1
    local seed=$2
    local suffix=$3
    local extra_args="${4:-}"

    local name="${profile}"
    [ -n "$suffix" ] && name="${profile}_s${suffix}"

    local img="$SCENARIOS_DIR/${name}.img"
    local manifest="$SCENARIOS_DIR/${name}_truth.json"

    echo -n "  ${name}: "

    local pass_args=""
    if [ "$profile" = "none" ]; then
        pass_args="--passes 0"
    else
        pass_args="--profile $profile"
    fi

    "$CORRSIM" -s "$SOURCE_DIR" -o "$img" \
        --size $IMAGE_SIZE --file-count $FILE_COUNT --seed $seed --sim-ops $SIM_OPS \
        $pass_args $extra_args \
        --manifest "$manifest" -v 2>&1 | tail -1
}

# Generate core scenarios (multiple seeds)
for scenario in $CORE_SCENARIOS; do
    echo "--- $scenario ---"
    for seed in $SEEDS; do
        generate_scenario "$scenario" "$seed" "$seed"
    done
    # Create a symlink for the primary seed (for backward compat with benchmark scripts)
    primary_seed=$(echo $SEEDS | awk '{print $1}')
    local_name="${scenario}"
    [ "$scenario" = "metadata-only" ] && local_name="metadata"
    [ "$scenario" = "bitrot-only" ] && local_name="bitrot"
    ln -sf "${scenario}_s${primary_seed}.img" "$SCENARIOS_DIR/${local_name}.img" 2>/dev/null || true
    ln -sf "${scenario}_s${primary_seed}_truth.json" "$SCENARIOS_DIR/${local_name}_truth.json" 2>/dev/null || true
    echo ""
done

# Generate targeted scenarios (primary seed only)
primary_seed=$(echo $SEEDS | awk '{print $1}')
for scenario in $TARGETED_SCENARIOS; do
    echo "--- $scenario ---"
    generate_scenario "$scenario" "$primary_seed" ""
    echo ""
done

echo "=== Done ==="
ls -lh "$SCENARIOS_DIR"/*.img 2>/dev/null | head -20
echo "Total: $(ls "$SCENARIOS_DIR"/*.img 2>/dev/null | wc -l) images"
