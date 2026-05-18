#!/usr/bin/env python3
"""
benchmark.py - Evaluate recovery quality against ground truth.

Composite scoring: 20% parseability + 30% binary similarity + 50% pixel similarity.
Categories: perfect (>=0.95), high (0.80-0.95), medium (0.50-0.80), low (0.20-0.50), missed (<0.20).

Used as a library by full_bench.sh and test_features.sh.
Also runnable standalone: benchmark.py <result_dir> <manifest> <source_dir>
"""

import os
import sys
import json
import hashlib


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(1024*1024)
            if not chunk: break
            h.update(chunk)
    return h.hexdigest()

def trim_to_eoi(data):
    """Trim JPEG data at last EOI marker (FF D9). Returns original if none found."""
    idx = data.rfind(b'\xff\xd9')
    return data[:idx+2] if idx >= 0 else data


def _compare_bytes(data_a, data_b):
    """Compare two byte buffers. Returns fraction of matching bytes (0.0-1.0)."""
    min_len = min(len(data_a), len(data_b))
    max_len = max(len(data_a), len(data_b))
    if max_len == 0:
        return 0.0

    try:
        import numpy as np
        a = np.frombuffer(data_a[:min_len], dtype=np.uint8)
        b = np.frombuffer(data_b[:min_len], dtype=np.uint8)
        matching = int(np.sum(a == b))
    except ImportError:
        a = memoryview(data_a)[:min_len]
        b = memoryview(data_b)[:min_len]
        matching = sum(x == y for x, y in zip(a, b))

    return matching / max_len


def byte_similarity(path_a, path_b):
    """Compare two files byte-by-byte with EOI trimming.
    Trims both files at their last JPEG EOI marker before comparing,
    so trailing cluster padding doesn't penalize the score."""
    with open(path_a, 'rb') as fa, open(path_b, 'rb') as fb:
        data_a = trim_to_eoi(fa.read())
        data_b = trim_to_eoi(fb.read())

    return _compare_bytes(data_a, data_b)


# Cache for source file hashes (computed once, reused across calls)
_source_cache = {}

def _get_source_cache(source_dir):
    """Build or return cached source file hash map."""
    if source_dir in _source_cache:
        return _source_cache[source_dir]
    source_by_hash = {}
    if source_dir and os.path.isdir(source_dir):
        for f in sorted(os.listdir(source_dir)):
            path = os.path.join(source_dir, f)
            if os.path.isfile(path):
                h = sha256(path)
                source_by_hash[h] = path
    _source_cache[source_dir] = source_by_hash
    return source_by_hash

def check_parseable(path):
    """Check if a JPEG file can be decoded. Returns 0.0, 0.5, or 1.0."""
    try:
        from PIL import Image
        img = Image.open(path)
        img.load()  # Force full decode
        return 1.0
    except Exception:
        try:
            with open(path, 'rb') as f:
                header = f.read(2)
            return 0.5 if header == b'\xff\xd8' else 0.0
        except:
            return 0.0


def pixel_similarity(path_a, path_b):
    """Compare two images by decoding and measuring normalized absolute pixel distance.
    Both images are decoded at the source dimensions. If the recovered image is
    smaller or fails to decode fully, the missing pixels show as gray fill, which
    naturally penalizes the score.

    Returns 1.0 (identical) to 0.0 (maximally different).
    Score = 1 - mean(abs(source_px - recovered_px)) / 255
    """
    try:
        from PIL import Image
        import numpy as np

        img_a = Image.open(path_a).convert('RGB')
        # Decode recovered image - Pillow gray-fills past corruption
        img_b = Image.open(path_b).convert('RGB')
    except Exception:
        return 0.0

    src_w, src_h = img_a.size
    if src_w == 0 or src_h == 0:
        return 0.0

    # Resize recovered to match source dimensions (handles truncated/different size)
    if img_b.size != img_a.size:
        img_b = img_b.resize((src_w, src_h), Image.LANCZOS)

    a = np.array(img_a, dtype=np.float32)
    b = np.array(img_b, dtype=np.float32)

    # Normalized mean absolute error across all pixels and channels
    score = 1.0 - np.mean(np.abs(a - b)) / 255.0
    return float(max(0.0, score))


def composite_score(binary_sim, parseable, pixel_sim):
    """Compute weighted composite recovery quality score."""
    return 0.20 * parseable + 0.30 * binary_sim + 0.50 * pixel_sim


def curved_score(raw, gamma=0.3):
    """Apply power curve to amplify differences near 1.0.
    gamma < 1 makes the curve steeper near 1.0.
    Examples with gamma=0.3:
      0.99 -> 0.997, 0.95 -> 0.985, 0.90 -> 0.969, 0.50 -> 0.812
    """
    if raw <= 0: return 0.0
    return raw ** gamma


def aggregate_score(results):
    """Single number combining recovery rate, quality, and false positive penalty.
    Range: 0.0 (nothing recovered) to ~1.0 (everything perfect, no FPs).
    """
    total_gt = results['total_ground_truth']
    if total_gt == 0: return 0.0

    # Recovery rate: fraction of GT files recovered at all (composite > 0.20)
    recovered = total_gt - results['missed']
    recovery_rate = recovered / total_gt

    # Quality: average composite among recovered files
    quality = results['avg_composite'] if results['avg_composite'] > 0 else 0

    # False positive penalty: proportional to FP count relative to GT
    fp_rate = results['false_positives'] / max(total_gt, 1)
    fp_penalty = min(fp_rate * 0.1, 0.05)  # cap at 5%

    return max(0.0, recovery_rate * quality - fp_penalty)


def drill_down_scores(results):
    """Compute detailed per-aspect scores for understanding feature impact.
    All scores are 0.0-1.0 fractions (not percentages)."""
    total = results['total_ground_truth']
    if total == 0:
        return {'d_recovery': 0, 'd_perfect': 0, 'd_quality': 0,
                'd_precision': 0, 'd_f1': 0, 'd_aggregate': 0, 'd_curved': 0}

    recovered = total - results['missed']
    d_recovery = recovered / total
    d_perfect = results['perfect'] / total
    d_quality = results.get('avg_composite', 0)

    # Precision: recovered / (recovered + FPs)
    total_output = results['total_recovered']
    d_precision = recovered / max(total_output, 1)

    # F1: harmonic mean of recovery rate and precision
    if d_recovery + d_precision > 0:
        d_f1 = 2 * d_recovery * d_precision / (d_recovery + d_precision)
    else:
        d_f1 = 0

    d_agg = aggregate_score(results)

    return {
        'd_recovery': d_recovery,
        'd_perfect': d_perfect,
        'd_quality': d_quality,
        'd_precision': d_precision,
        'd_f1': d_f1,
        'd_aggregate': d_agg,
        'd_curved': curved_score(d_agg),
    }



def load_manifest(manifest_path):
    """Load ground truth manifest from corruption simulator."""
    with open(manifest_path) as f:
        data = json.load(f)
    return data

def compare_results(output_dir, manifest, source_dir):
    """Compare recovered files against ground truth using byte-level similarity."""

    # Load ground truth files and their source paths (as list to preserve duplicates)
    gt_files = []
    for record in manifest.get('files', []):
        gt_files.append({
            'sha256': record['sha256'],
            'image_path': record.get('image_path', ''),
            'size': record.get('file_size', record.get('size', 0)),
            'source_path': record.get('original', record.get('source_path', '')),
            'deleted': False,
        })
    # Also add deleted files
    for record in manifest.get('deleted_files', []):
        gt_files.append({
            'sha256': record['sha256'],
            'image_path': record.get('image_path', ''),
            'size': record.get('file_size', record.get('size', 0)),
            'source_path': record.get('original', record.get('source_path', '')),
            'deleted': True,
        })

    # Build map: source file hash -> path (cached across calls)
    source_by_hash = _get_source_cache(source_dir)

    # Collect recovered files
    recovered = []
    files_dir = os.path.join(output_dir, 'files')
    if os.path.isdir(files_dir):
        for f in sorted(os.listdir(files_dir)):
            path = os.path.join(files_dir, f)
            if os.path.isfile(path):
                recovered.append({
                    'name': f,
                    'hash': sha256(path),
                    'size': os.path.getsize(path),
                    'path': path,
                })

    # For each ground truth file, find the best matching recovered file
    gt_matches = []
    used_recovered = set()

    for gt_info in gt_files:
        gt_hash = gt_info['sha256']
        # Find the source file on disk for byte comparison
        src_path = gt_info['source_path']
        if not src_path or not os.path.isfile(src_path):
            # Try to find by hash in source dir
            src_path = source_by_hash.get(gt_hash, '')

        best_match = None
        best_sim = 0.0

        # First check for perfect hash match
        for i, rec in enumerate(recovered):
            if i in used_recovered:
                continue
            if rec['hash'] == gt_hash:
                best_match = {'index': i, 'sim': 1.0, 'name': rec['name'], 'method': 'sha256'}
                break

        # If no perfect match and we have the source file, do byte comparison.
        # Only compare against recovered files with similar size (within 3x).
        if not best_match and src_path and os.path.isfile(src_path):
            gt_size = gt_info['size']
            src_data = None  # lazy load
            for i, rec in enumerate(recovered):
                if i in used_recovered:
                    continue
                if gt_size > 0:
                    size_ratio = rec['size'] / gt_size
                    if size_ratio < 0.3 or size_ratio > 3.0:
                        continue
                # Lazy load source data once (trimmed at EOI)
                if src_data is None:
                    with open(src_path, 'rb') as f:
                        src_data = trim_to_eoi(f.read())
                # Inline fast comparison with EOI trimming
                with open(rec['path'], 'rb') as f:
                    rec_data = trim_to_eoi(f.read())
                sim = _compare_bytes(src_data, rec_data)
                if sim > best_sim:
                    best_sim = sim
                    best_match = {'index': i, 'sim': sim, 'name': rec['name'], 'method': 'bytes'}

        if best_match and best_match['sim'] >= 0.1:  # at least 10% similar
            used_recovered.add(best_match['index'])

            # Compute composite score components
            rec_path = recovered[best_match['index']]['path']
            bin_sim = best_match['sim']

            # For perfect binary match, skip expensive pixel comparison
            if bin_sim >= 0.99:
                parse_score = 1.0
                pix_sim = 1.0
            else:
                parse_score = check_parseable(rec_path)
                if src_path and os.path.isfile(src_path):
                    pix_sim = pixel_similarity(src_path, rec_path)
                else:
                    pix_sim = 0.0

            comp = composite_score(bin_sim, parse_score, pix_sim)

            gt_matches.append({
                'gt_hash': gt_hash,
                'gt_path': gt_info['image_path'],
                'gt_size': gt_info['size'],
                'deleted': gt_info['deleted'],
                'recovered_name': best_match['name'],
                'similarity': bin_sim,
                'composite': comp,
                'parseable': parse_score,
                'pixel_sim': pix_sim,
                'match_method': best_match['method'],
            })
        else:
            gt_matches.append({
                'gt_hash': gt_hash,
                'gt_path': gt_info['image_path'],
                'gt_size': gt_info['size'],
                'deleted': gt_info['deleted'],
                'recovered_name': None,
                'similarity': 0.0,
                'composite': 0.0,
                'parseable': 0.0,
                'pixel_sim': 0.0,
                'match_method': 'none',
            })

    # Classify by composite score
    perfect = sum(1 for m in gt_matches if m['composite'] >= 0.95)
    high = sum(1 for m in gt_matches if 0.80 <= m['composite'] < 0.95)
    medium = sum(1 for m in gt_matches if 0.50 <= m['composite'] < 0.80)
    low = sum(1 for m in gt_matches if 0.20 <= m['composite'] < 0.50)
    missed = sum(1 for m in gt_matches if m['composite'] < 0.20)

    comps = [m['composite'] for m in gt_matches if m['composite'] >= 0.2]
    avg_composite = sum(comps) / len(comps) if comps else 0

    sims = [m['similarity'] for m in gt_matches if m['similarity'] >= 0.1]
    avg_sim = sum(sims) / len(sims) if sims else 0

    false_positives = [recovered[i]['name'] for i in range(len(recovered)) if i not in used_recovered]

    total_gt = len(gt_files)
    deleted_gt = sum(1 for v in gt_files if v.get('deleted', False))

    result = {
        'total_ground_truth': total_gt,
        'deleted_files': deleted_gt,
        'active_files': total_gt - deleted_gt,
        'total_recovered': len(recovered),
        'perfect': perfect,         # composite >= 0.95
        'high_quality': high,       # 0.80-0.95
        'medium_quality': medium,   # 0.50-0.80
        'low_quality': low,         # 0.20-0.50
        'missed': missed,           # < 0.20
        'false_positives': len(false_positives),
        'avg_similarity': avg_sim,
        'avg_composite': avg_composite,
        'recovery_rate': (total_gt - missed) / total_gt * 100 if total_gt > 0 else 0,
        'perfect_rate': perfect / total_gt * 100 if total_gt > 0 else 0,
        'matches': sorted(gt_matches, key=lambda m: -m['composite']),
        'false_positive_files': false_positives[:10],
    }

    result.update(drill_down_scores(result))
    return result

def print_results(r):
    """Pretty-print evaluation results."""
    print(f"  Perfect (>=0.95):  {r['perfect']}")
    print(f"  High (0.80-0.95):  {r['high_quality']}")
    print(f"  Medium (0.50-0.80):{r['medium_quality']}")
    print(f"  Low (0.20-0.50):   {r['low_quality']}")
    print(f"  Missed (<0.20):    {r['missed']}")
    print(f"  False pos:         {r['false_positives']}")
    print(f"  Avg binary sim:    {r['avg_similarity']:.3f}")
    print(f"  Avg composite:     {r['avg_composite']:.3f}")
    print(f"  Aggregate:         {r['d_aggregate']:.4f}")
    print(f"  Curved:            {r['d_curved']:.4f}")


def main():
    """Evaluate a recovery result directory against ground truth.
    Usage: benchmark.py <result_dir> <manifest> <source_dir>"""
    if len(sys.argv) < 4:
        print("Usage: benchmark.py <result_dir> <manifest> <source_dir>")
        print("  Evaluates recovered files against ground truth with composite scoring.")
        sys.exit(1)

    result_dir = sys.argv[1]
    manifest_path = sys.argv[2]
    source_dir = sys.argv[3]

    m = load_manifest(manifest_path)
    r = compare_results(result_dir, m, source_dir)
    print_results(r)


if __name__ == '__main__':
    main()
