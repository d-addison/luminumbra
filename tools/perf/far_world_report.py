#!/usr/bin/env python3
"""Report pinned Ladder qualification receipts; stdlib only, no engine launch.

Each --receipt names a root-relative JSON object with schema
luminumbra.far_world_receipt.v1 and benchmark, stations and refresh pins. A pin
contains artifact (root-relative) and sha256. Optional traversal is also pinned.
Refresh evidence uses schema luminumbra.far_world_refresh.v1, with samples bound
by world_epoch, committed_generation, target_serial and manifest_fingerprint
(decimal string). Every sample supplies measured serving_target_overlap_bytes
and refresh_latency_ms. These measurements are required, never inferred from
station settle time or asynchronously sampled pools. Missing instrumentation
fails qualification. This tool neither chooses limits nor measures hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gates'))

from fixture_paths import read_bytes, reference_parts
from far_ladder_oracle import STATIONS, no_duplicates, validate_artifact
import perf
import render_contract

PRESETS = ('default', 'mountains', 'archipelago', 'cave_dense')
# File-only diagnostic bounds: four runs, 64 MiB per artifact, 18000 frames or
# refresh samples and five horizon stations per run. No engine limit changes.
MAX_BYTES = 64 * 1024 * 1024
MAX_SAMPLES = 18000
WORK = ('source_read_bytes', 'source_copied_bytes', 'source_decode_work',
        'capture_work', 'admission_work', 'admission_height_observations')
PUBLICATION = ('world_epoch', 'committed_generation', 'target_serial', 'manifest_fingerprint')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def uint(value, label, minimum=0):
    require(type(value) is int and minimum <= value < 2**64, f'invalid {label}')
    return value


def publication(row):
    for key in PUBLICATION[:-1]:
        uint(row.get(key), key, int(key != 'committed_generation'))
    fingerprint = row.get('manifest_fingerprint')
    require(perf._far_uint64_identity(fingerprint) and int(fingerprint) > 0,
            'missing publication manifest fingerprint')
    return tuple(row[key] for key in PUBLICATION[:-1]) + (int(fingerprint),)


def nonfinite(value):
    raise ValueError(f'nonfinite JSON value: {value}')


def document(root, reference, pin=None):
    path = root.joinpath(*reference_parts(reference))
    raw = read_bytes(root, path, limit=MAX_BYTES)
    digest = hashlib.sha256(raw).hexdigest()
    require(pin is None or pin == digest, 'evidence sha256 mismatch')
    data = json.loads(raw.decode('utf-8-sig'), object_pairs_hook=no_duplicates,
                      parse_constant=nonfinite)
    require(isinstance(data, dict), 'evidence must be an object')
    return data, digest


def pinned(root, receipt, key):
    pin = receipt.get(key)
    require(isinstance(pin, dict) and set(pin) == {'artifact', 'sha256'} and
            isinstance(pin['sha256'], str) and re.fullmatch('[0-9a-f]{64}', pin['sha256']),
            f'missing/invalid {key} pin')
    return document(root, pin['artifact'], pin['sha256'])


def session(row, snapshot=False):
    require(row.get('authority_kind') == 'session', 'session authority required')
    # Reuse the approved benchmark authority contract for station metadata too.
    normalized = dict(row)
    if snapshot:
        normalized.update(content_identity=str(row.get('content_identity')),
                          manifest_fingerprint=str(row.get('manifest_fingerprint')), frame=row.get('frame_id'))
    perf._validate_far_authority(normalized, ladder_baseline=True)
    key = publication(normalized)
    require(normalized.get('refresh_work') is not None, 'missing source/refresh work samples')
    return key, normalized['refresh_work']


def summarize(values, unit):
    return {k: v for k, v in perf.summarize(values, unit).items() if k != 'samples'}


def report_run(root, receipt, qualified_renderer):
    require(receipt.get('schema') == 'luminumbra.far_world_receipt.v1', 'wrong receipt schema')
    benchmark, benchmark_hash = pinned(root, receipt, 'benchmark')
    stations, stations_hash = pinned(root, receipt, 'stations')
    refresh, refresh_hash = pinned(root, receipt, 'refresh')
    traversal_bytes = None
    if 'traversal' in receipt:
        pin = receipt['traversal']
        require(isinstance(pin, dict) and set(pin) == {'artifact', 'sha256'} and
                isinstance(pin['sha256'], str) and re.fullmatch('[0-9a-f]{64}', pin['sha256']),
                'invalid traversal pin')
        traversal = root.joinpath(*reference_parts(pin['artifact']))
        traversal_bytes = read_bytes(root, traversal, limit=1024 * 1024)
        require(hashlib.sha256(traversal_bytes).hexdigest() == pin['sha256'], 'traversal sha256 mismatch')
    if traversal_bytes is None:
        render_contract.validate(benchmark, qualified_renderer, ladder_baseline=True)
    else:
        # The existing validator accepts a path. Give it a private snapshot of
        # the verified bytes, never reopen the externally supplied reference.
        with tempfile.TemporaryDirectory() as directory:
            snapshot_path = Path(directory) / 'traversal.txt'
            snapshot_path.write_bytes(traversal_bytes)
            render_contract.validate(benchmark, qualified_renderer, traversal=snapshot_path, ladder_baseline=True)
    workload = benchmark['workload']
    preset = workload['preset']
    require(preset in PRESETS and stations.get('world_preset') == preset, 'wrong preset')
    require(type(stations.get('stations')) is list and len(stations['stations']) == 5,
            'missing station samples')
    validate_artifact(stations, 'ladder', required_stations=STATIONS)
    works = {}
    publications = {}
    benchmark_keys = set()
    previous = None
    for row in benchmark['far_world']['frames']:
        key, work = session(row)
        require(previous is None or (key[0] == previous[0] and key[1] >= previous[1]
                and key[2] >= previous[2]), 'regressed benchmark publication')
        previous = key
        require((key[0], key[2]) not in publications or publications[(key[0], key[2])] == (key[1], key[3]),
                'changed publication manifest')
        publications[(key[0], key[2])] = (key[1], key[3])
        benchmark_keys.add(key)
        require(key not in works or works[key] == work, 'changed work for repeated publication')
        works[key] = work
    station_keys = set()
    for row in stations['stations']:
        key, work = session(row['snapshot'], snapshot=True)
        require(key[0] == previous[0], 'station/benchmark world epoch disagreement')
        require((key[0], key[2]) not in publications or publications[(key[0], key[2])] == (key[1], key[3]),
                'changed publication manifest')
        publications[(key[0], key[2])] = (key[1], key[3])
        require(key not in works or works[key] == work, 'station/benchmark work disagreement')
        works[key] = work
        station_keys.add(key)
    require(station_keys & benchmark_keys,
            'station and benchmark publications do not overlap')
    require(refresh.get('schema') == 'luminumbra.far_world_refresh.v1', 'wrong refresh schema')
    samples = refresh.get('samples')
    require(type(samples) is list and 1 <= len(samples) <= MAX_SAMPLES, 'missing refresh samples')
    measurements = {}
    for sample in samples:
        require(isinstance(sample, dict), 'invalid refresh sample')
        key = publication(sample)
        require(key in works and key not in measurements, 'unknown/duplicate refresh publication')
        overlap = uint(sample.get('serving_target_overlap_bytes'), 'overlap bytes')
        latency = render_contract.number(sample.get('refresh_latency_ms'), 'refresh latency')
        measurements[key] = (overlap, latency)
    require(measurements.keys() == works.keys(), 'missing publication refresh measurement')
    ordered = sorted(works)
    pools = []
    for tier in range(5):
        observations = [row['snapshot']['tiers'][tier] for row in stations['stations']]
        pools.append(dict(tier=tier + 1, peak_bytes=max(row['pool_peak_bytes'] for row in observations),
                          sampled_total_bytes=max(row['pool_sampled_total_bytes'] for row in observations)))
    return dict(preset=preset, preset_revision=workload['preset_revision'],
                preset_identity=workload['preset_identity'], seed=workload['seed'],
                adapter=benchmark['adapter'],
                evidence=dict(benchmark=benchmark_hash, stations=stations_hash, refresh=refresh_hash),
                source_work={key: summarize([works[k][key] for k in ordered], 'bytes' if key.endswith('bytes') else 'operations')
                             for key in WORK},
                source_ms=summarize([works[k]['source_ms'] for k in ordered], 'ms'),
                pool_peaks=pools, memory_sampling='asynchronous_diagnostic_not_atomic_admission',
                gpu_bytes_kind='logical_stores',
                serving_target_overlap_bytes=summarize([measurements[k][0] for k in ordered], 'bytes'),
                refresh_latency_ms=summarize([measurements[k][1] for k in ordered], 'ms'),
                station_settle_seconds=summarize([row['settle_seconds'] for row in stations['stations']], 'seconds'),
                distributions={metric: benchmark['distribution'][metric] for metric in render_contract.METRICS})


def build_report(root, references, qualified_renderer):
    require(1 <= len(references) <= len(PRESETS), 'expected at most four receipts')
    runs, failures = {}, []
    for reference in sorted(references):
        try:
            receipt, _ = document(root, reference)
            run = report_run(root, receipt, qualified_renderer)
            require(run['preset'] not in runs, 'duplicate preset receipt')
            runs[run['preset']] = run
        except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError, RecursionError) as error:
            failures.append(dict(receipt=reference, reason=str(error)))
    for preset in PRESETS:
        if preset not in runs:
            failures.append(dict(preset=preset, reason='missing complete session receipt'))
    return dict(schema='luminumbra.far_world_report.v1', passed=not failures,
                runs=[runs[preset] for preset in PRESETS if preset in runs], failures=failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--receipt', action='append', required=True)
    parser.add_argument('--qualified-renderer', required=True)
    args = parser.parse_args()
    try:
        result = build_report(args.root, args.receipt, args.qualified_renderer)
    except ValueError as error:
        parser.exit(1, f'far world report refused: {error}\n')
    print(json.dumps(result, sort_keys=True, indent=2, allow_nan=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
