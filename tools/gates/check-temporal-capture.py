#!/usr/bin/env python3
"""Measure an explicit E1c sequence. JSON is advisory; invalid input alone fails.

Usage: check-temporal-capture.py MANIFEST [--rois D2_ROIS]
Optional temporal evidence is documented in visual-diagnostics.md. Unknown or
absent evidence yields ineligible results, never inferred lighting or identity.
"""
from array import array
import argparse
import json
import math
import os
from pathlib import Path
import runpy
import sys

D = runpy.run_path(str(Path(__file__).with_name('check-diagnostic-capture.py')))
A = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'timelapse.py'))
require, finite = D['require'], D['finite']
METRICS = ('delta', 'flicker', 'pops', 'lighting_continuity', 'seam_persistence')
CLASSES = ('static', 'animal', 'rain', 'wind_foliage', 'lightning', 'cloud_shadow_edge',
           'motion', 'disocclusion', 'weather')
# Two packed frames (24 bytes/pixel each), <=256 MiB metadata/accumulator allowance.
STAGING = 512 * 1024 * 1024
MAX_GROUPS = 1024
MAX_WINDOW_SAMPLES = 65536


class Histogram:
    """256 fixed bins; values above one retain a conservative exact maximum."""
    def __init__(self):
        self.bins = [0] * 256
        self.count, self.total, self.maximum = 0, 0., 0.

    def add(self, value, count=1):
        self.bins[min(255, int(min(value, 1.) * 256))] += count
        self.count += count
        self.total += value * count
        self.maximum = max(self.maximum, value)

    def percentile(self, fraction):
        target, accumulated = math.ceil(self.count * fraction), 0
        for index, count in enumerate(self.bins):
            accumulated += count
            if accumulated >= target:
                return self.maximum if index == 255 else min(self.maximum, (index + 1) / 256)
        return None

    def values(self):
        return dict(mean=self.total / self.count, p95_upper=self.percentile(.95),
                    p99_upper=self.percentile(.99), maximum=self.maximum)


def luminance(rgb):
    return sum(v * w for v, w in zip(rgb, (.2126, .7152, .0722)))


def identity(draw, meshes):
    """Capture-local numeric IDs never enter a cross-frame key.

    LOD and ownership are transition attributes, retained separately. Ambiguous
    producer keys are unavailable rather than merged into a guessed identity.
    """
    mesh = meshes[draw['mesh'] - 1]
    if not draw['supported'] or not mesh['path'] or mesh['provenance'] == 4:
        return None
    return (tuple(draw['chunk']), mesh['path'], mesh['provenance'],
            draw['pass'], draw['shader_variant'])


def evidence(capture, table):
    value = capture.get('temporal')
    if not isinstance(value, dict) or type(value.get('version')) is not int or value['version'] != 1:
        return {}
    # Unknown metadata is unavailable, including unknown enum/category values.
    if not set(value) <= {'version', 'classes', 'steady_light', 'exposure', 'smooth_exposure',
                          'smooth_luminance', 'solar_luminance', 'phase', 'labels', 'seams'}:
        return {}
    classes = value.get('classes')
    if not isinstance(classes, dict) or len(classes) != len(table['draws']) or any(str(d['id']) not in classes
            or classes[str(d['id'])] not in CLASSES for d in table['draws']):
        return {}
    return value


def attribution(key, group):
    return dict(chunk=list(key[0]), asset=key[1], provenance=key[2], producer_pass=key[3],
                shader_variant=key[4], lod=group['lod'], ownership=group['ownership'],
                classification=group['classification'])


def frame_data(meta, capture, table, streams, offsets, rois):
    width, height = meta['width'], meta['height']
    require(width * height * 48 + 256 * 1024 * 1024 <= STAGING, 'temporal staging budget')
    require(len(table['draws']) * len(rois) <= MAX_GROUPS, 'participating producer/ROI budget')
    temporal = evidence(capture, table)
    keys = {draw['id']: identity(draw, table['meshes']) for draw in table['draws']}
    counts = {}
    for key in keys.values():
        counts[key] = counts.get(key, 0) + 1
    keys = {draw: key if key is not None and counts[key] == 1 else None
            for draw, key in keys.items()}
    channel = 'lighting' if 'lighting' in streams else 'albedo'
    packed = array('f')
    draws = array('I')
    groups = [{} for _ in rois]
    rects = [D['rectangle'](r['rect'], width, height) for r in rois]
    required = (channel, 'depth', 'material', 'ids')
    optional = tuple(c for c in ('expected_mask', 'reference', 'albedo') if c in streams and c not in required)
    for y in range(height):
        rows = {c: D['pixels_row'](streams[c], offsets[c], width, height,
                4 if c == 'ids' else dict(D['PLANES'], **D['CHANNELS'])[c],
                y, 0, width, c == 'ids') for c in required + optional}
        for x in range(width):
            rgb, depth, material, pixel = (rows[c][x] for c in required)
            require(all(finite(v) for v in rgb) and 0 <= depth[0] <= 1,
                    'invalid temporal color/depth')
            packed.extend((*rgb, depth[0], material[0]))
            mask = rows['expected_mask'][x][0] if 'expected_mask' in rows else None
            require(mask is None or mask in (0., 1.), 'binary expected mask required')
            draws.append(pixel[0] if mask != 0 else 0)
            key = keys.get(pixel[0])
            if mask == 0:
                continue
            if key is None:
                continue
            draw = table['draws'][pixel[0] - 1]
            for region, (left, top, right, bottom) in enumerate(rects):
                if not (left <= x < right and top <= y < bottom):
                    continue
                group = groups[region].get(key)
                if group is None:
                    group = dict(count=0, rgb=[0., 0., 0.], depth=0., material=0.,
                                 residual=0., seam_count=0, mask_count=0, lod=draw['lod'],
                                 ownership=draw['ownership'],
                                 classification=temporal.get('classes', {}).get(str(pixel[0])))
                    groups[region][key] = group
                group['count'] += 1
                for c, v in enumerate(rgb):
                    group['rgb'][c] += v
                group['depth'] += depth[0]
                group['material'] += material[0]
                if 'expected_mask' in rows:
                    mask = rows['expected_mask'][x][0]
                    require(mask in (0., 1.), 'binary expected mask required')
                    if mask:
                        group['mask_count'] += 1
                        if 'reference' in rows:
                            residual = max(abs(v - ref) for v, ref in zip(rows.get('albedo', rows[channel])[x], rows['reference'][x]))
                            group['residual'] += residual
                            group['seam_count'] += residual > 0
    for region in groups:
        for group in region.values():
            n = group['count']
            group['rgb'] = [v / n for v in group['rgb']]
            group['depth'] /= n
            group['material'] /= n
    return dict(width=width, height=height, camera=meta['camera'], channel=channel,
                temporal=temporal, groups=groups, rects=rects, keys=keys,
                pixels=packed, draws=draws, seam_channels=all(c in streams for c in
                ('expected_mask', 'reference')))


def result(kind, roi):
    return dict(metric=kind, roi=roi['name'], status='ineligible', reason='insufficient usable samples',
                coverage=0., samples=0, units=dict(delta='linear RGB/luminance; hardware depth; fraction',
                flicker='exposed linear luminance', pops='linear luminance; fraction',
                lighting_continuity='linear luminance/tick; exposure/tick; per tick squared',
                seam_persistence='samples; sim ticks; fraction')[kind], masks=list(CLASSES[1:]),
                threshold_version=None, baseline_version=None, values=None, events=[])


def event(record, score, frame, roi, values):
    record['events'].append(dict(score=score, frame_index=frame['index'], tick=frame['tick'],
                                roi=roi['name'], **values))
    record['events'].sort(key=lambda e: (-e['score'], e['frame_index'], e['roi']))
    del record['events'][8:]
    worst = record['events'][0]
    record['values'] = {k: v for k, v in worst.items() if k not in ('score', 'frame_index', 'tick', 'roi')}
    record['worst_sample'] = dict(index=worst['frame_index'], tick=worst['tick'])
    record.update(status='advisory', reason=None)


def usable(group):
    return group['classification'] == 'static'


def adjacent(previous, current, frame, before, roi, region, records):
    old, new = previous['groups'][region], current['groups'][region]
    common = sorted(set(old) & set(new))
    rect = current['rects'][region]
    left, top, right, bottom = rect
    area = (right - left) * (bottom - top)
    delta, pops = records['delta'], records['pops']
    if not common or not previous['temporal'] or not current['temporal']:
        for record in (delta, pops):
            record['reason'] = 'missing/unknown attribution or no persistent producer coverage'
        return
    dt = frame['tick'] - before['tick'] + (frame['sub_tick_60'] - before['sub_tick_60']) / 60
    hist, raw, lum = Histogram(), Histogram(), Histogram()
    rgb_hist = [Histogram() for _ in range(3)]
    depth_hist = Histogram()
    changed = depth_changed = material_changed = ownership_changed = 0
    class_hists = {name: Histogram() for name in CLASSES[1:]}
    trend = (previous['temporal'].get('smooth_luminance'),
             current['temporal'].get('smooth_luminance'))
    trend_valid = all(finite(v) and 0 <= v <= 3.4028235e38 for v in trend)
    pop_stats = {}
    # The unmasked spatial companion preserves motion/disocclusion changes.
    # Persistent maps require the same producer at each pixel; reordering uses
    # that producer's mean, never the unrelated color formerly at this pixel.
    width = current['width']
    for y in range(top, bottom):
        for x in range(left, right):
            index = y * width + x
            start = index * 5
            rgb = current['pixels'][start:start + 3]
            prev_rgb = previous['pixels'][start:start + 3]
            raw.add(max(abs(a - b) for a, b in zip(rgb, prev_rgb)))
            key = current['keys'].get(current['draws'][index])
            if key not in old or key not in new:
                continue
            same = previous['keys'].get(previous['draws'][index]) == key
            base = prev_rgb if same else old[key]['rgb']
            value = max(abs(a - b) for a, b in zip(rgb, base))
            category = new[key]['classification']
            if category != old[key]['classification'] or category not in CLASSES:
                continue
            if trend_valid:
                stats = pop_stats.get(key)
                if stats is None:
                    stats = dict(histogram=Histogram(), changed=0, magnitude=0.)
                    pop_stats[key] = stats
                magnitude = abs(luminance(rgb) - luminance(base))
                residual = abs((luminance(rgb) - trend[1]) - (luminance(base) - trend[0]))
                stats['histogram'].add(residual)
                stats['changed'] += residual > 0
                stats['magnitude'] = max(stats['magnitude'], magnitude)
            if category != 'static':
                class_hists[category].add(value)
                continue
            hist.add(value)
            for component, component_hist in enumerate(rgb_hist):
                component_hist.add(abs(rgb[component] - base[component]))
            depth_hist.add(abs(current['pixels'][start + 3] -
                           (previous['pixels'][start + 3] if same else old[key]['depth'])))
            lum.add(abs(luminance(rgb) - luminance(base)))
            changed += value > 0
            depth_changed += (current['pixels'][start + 3] !=
                (previous['pixels'][start + 3] if same else old[key]['depth']))
            material_changed += (current['pixels'][start + 4] !=
                (previous['pixels'][start + 4] if same else old[key]['material']))
            ownership_changed += new[key]['ownership'] != old[key]['ownership']
    if hist.count:
        delta['samples'] += 1
        delta['coverage'] = max(delta['coverage'], hist.count / area)
        event(delta, hist.maximum, frame, roi, dict(delta_ticks=dt, rgb=hist.values(),
              luminance=lum.values(), rgb_components=[h.values() for h in rgb_hist],
              depth=depth_hist.values(), changed_area=changed / area,
              depth_changed_area=depth_changed / area, material_changed_area=material_changed / area,
              ownership_changed_area=ownership_changed / area, unmasked=raw.values(),
              classified={k: v.values() for k, v in class_hists.items() if v.count},
              channel=current['channel']))
    if raw.count:
        companion = dict(unmasked=raw.values(),
                         classified={k: v.values() for k, v in class_hists.items() if v.count})
        if raw.maximum > delta.get('companion_maximum', -1):
            delta['companions'] = companion
            delta['companion_maximum'] = raw.maximum
    if not hist.count:
        delta['reason'] = 'no persistent static surface coverage; classified companions are separate'
    # Raw jumps carry attribution, never an inferred LOD-defect verdict.
    for key in common:
        a, b = old[key], new[key]
        if a['classification'] != b['classification'] or b['classification'] not in CLASSES:
            continue
        if not trend_valid:
            pops['reason'] = 'missing smooth lighting trend'
            continue
        stats = pop_stats.get(key)
        if stats is None:
            continue
        residual = stats['histogram'].maximum
        pops['samples'] += 1
        pops['coverage'] = max(pops['coverage'], min(a['count'], b['count']) / area)
        transitions = [name for name in ('lod', 'ownership', 'depth', 'material') if a[name] != b[name]]
        event(pops, residual, frame, roi, dict(magnitude=stats['magnitude'], residual=residual,
              residual_distribution=stats['histogram'].values(),
              area=stats['changed'] / area, producer_area=b['count'] / area,
              delta_ticks=dt, transitions=transitions,
              interpretation='unexplained' if usable(b) else 'expected_' + b['classification'],
              before=attribution(key, a), after=attribution(key, b)))


def continuity(history, current, frame, roi, region, record):
    meta = current['temporal']
    phase = meta.get('phase')
    labels = meta.get('labels')
    valid_labels = isinstance(labels, dict) and set(labels) == {
        'horizon_crossing', 'cascade_change', 'cloud', 'strike'} and all(type(labels.get(k)) is bool for k in
                         ('horizon_crossing', 'cascade_change', 'cloud', 'strike'))
    groups = {k: v for k, v in current['groups'][region].items() if usable(v)}
    if (current['channel'] != 'lighting' or phase not in ('dawn', 'dusk') or not valid_labels
            or not groups or not all(finite(meta.get(k)) and 0 <= meta[k] <= 3.4028235e38 for k in
                                    ('exposure', 'solar_luminance'))):
        record['reason'] = 'valid lighting, solar progression, exposure, labels and static ROI required'
        history.clear()
        return
    # Three consecutive samples with a common static producer set; changing
    # coverage cannot masquerade as a derivative of the same terrain/sky.
    history.append((frame, meta, groups))
    del history[:-3]
    if len(history) < 3:
        return
    common = set.intersection(*(set(item[2]) for item in history))
    if not common or any(item[1]['phase'] != phase for item in history):
        record['reason'] = 'no persistent dusk/dawn surface'
        return
    intervals = [(history[i + 1][0]['tick'] - history[i][0]['tick']) +
                 (history[i + 1][0]['sub_tick_60'] - history[i][0]['sub_tick_60']) / 60
                 for i in range(2)]
    if min(intervals) <= 0:
        record['reason'] = 'lighting derivatives require positive tick intervals'
        return
    weights = {k: min(g[k]['count'] for _, _, g in history) for k in common}
    lights = [sum(luminance(g[k]['rgb']) * weights[k] for k in sorted(common)) /
              sum(weights.values()) for _, _, g in history]
    def derivatives(values):
        first = [(values[i + 1] - values[i]) / intervals[i] for i in range(2)]
        return dict(first=first, second=(first[1] - first[0]) / (sum(intervals) / 2))
    measured = derivatives(lights)
    expected = derivatives([m['solar_luminance'] for _, m, _ in history])
    exposure = derivatives([m['exposure'] for _, m, _ in history])
    residual = abs(measured['second'] - expected['second'])
    record['samples'] += 1
    left, top, right, bottom = current['rects'][region]
    record['coverage'] = max(record['coverage'], sum(groups[k]['count'] for k in common) /
                             ((right - left) * (bottom - top)))
    event(record, residual, frame, roi, dict(light=measured, expected_solar=expected,
          exposure=exposure, residual_second=residual, phase=phase, labels=labels,
          first_residual=[a - b for a, b in zip(measured['first'], expected['first'])]))


def flicker(window, current, frame, roi, region, record):
    # Explicit steady holds are the E1c 120-frame windows. Reset on gaps,
    # non-steady evidence, control/tick changes or repeated/out-of-order hold IDs.
    meta = current['temporal']
    signature = (frame['tick'], frame['control'])
    expected = window.get('next', 0)
    if (frame['kind'] != 1 or meta.get('steady_light') is not True
            or current['channel'] != 'lighting' or frame['hold_frame'] != expected
            or (expected and window.get('signature') != signature)
            or not all(finite(meta.get(k)) and 0 <= meta[k] <= 3.4028235e38 for k in
                       ('exposure', 'smooth_exposure', 'smooth_luminance'))):
        window.clear()
        record['reason'] = 'known steady 120-frame hold, lighting and smooth exposure trend required'
        if frame['hold_frame'] != 0 or frame['kind'] != 1:
            return
        # Invalid first-frame evidence must not start a usable window.
        if meta.get('steady_light') is not True or current['channel'] != 'lighting' or not all(
                finite(meta.get(k)) and 0 <= meta[k] <= 3.4028235e38 for k in
                ('exposure', 'smooth_exposure', 'smooth_luminance')):
            return
    window.update(next=frame['hold_frame'] + 1, signature=signature)
    tracks = window.setdefault('tracks', {})
    left, top, right, bottom = current['rects'][region]
    area = (right - left) * (bottom - top)
    valid_pixels = sum(g['count'] for g in current['groups'][region].values() if usable(g))
    for key in sorted(current['groups'][region]):
        group = current['groups'][region][key]
        if not usable(group) or valid_pixels < 64 or valid_pixels / area < .01:
            continue
        track = tracks.get(key)
        if track is None:
            track = dict(n=0, low=math.inf, high=-math.inf, histogram=Histogram(), last=None,
                         difference=None, alternating=0, coverage=1.,
                         surface=attribution(key, group), pixels=group['count'])
            tracks[key] = track
        window['samples'] = window.get('samples', 0) + 1
        require(window['samples'] <= MAX_WINDOW_SAMPLES, 'steady window producer sample budget')
        value = luminance(group['rgb']) * meta['exposure'] - meta['smooth_luminance'] * meta['smooth_exposure']
        difference = value - track['last'] if track['last'] is not None else None
        if difference is not None and track['difference'] is not None:
            track['alternating'] += difference * track['difference'] < 0
        track.update(last=value, difference=difference, n=track['n'] + 1,
                     low=min(track['low'], value), high=max(track['high'], value),
                     coverage=min(track['coverage'], valid_pixels / area),
                     pixels=min(track['pixels'], group['count']))
        track['histogram'].add(abs(value))
    if frame['hold_frame'] != 119:
        return
    record['reason'] = 'fewer than 30 usable samples or fewer than 64 pixels / 1% ROI coverage'
    persistent_pixels = sum(t['pixels'] for t in tracks.values() if t['n'] >= 30)
    if persistent_pixels < 64 or persistent_pixels / area < .01:
        window.clear()
        return
    for key, track in sorted(tracks.items()):
        if track['n'] < 30:
            continue
        record['samples'] += track['n']
        record['coverage'] = max(record['coverage'], persistent_pixels / area)
        event(record, track['high'] - track['low'], frame, roi, dict(
              peak_to_peak=track['high'] - track['low'], p95_amplitude_upper=track['histogram'].percentile(.95),
              alternating_sign_differences=track['alternating'], usable_samples=track['n'],
              window_frames=120, surface=track['surface']))
    window.clear()


def seam(state, current, frame, roi, region, record):
    pinned = [m for m in roi['metrics'] if m['kind'] in ('seam', 'boundary')]
    evidence_value = current['temporal'].get('seams', {})
    evidence_value = evidence_value.get(roi['name']) if isinstance(evidence_value, dict) else None
    if (not pinned or not current['seam_channels'] or not isinstance(evidence_value, dict)
            or not D['leaf'](evidence_value.get('id')) or not D['leaf'](evidence_value.get('mask_version'))
            or set(evidence_value) != {'id', 'mask_version', 'openings_excluded'}
            or evidence_value.get('openings_excluded') is not True):
        state.clear()
        record['reason'] = 'pinned seam/boundary, reference, versioned world seam and opening-excluded mask required'
        return
    groups = current['groups'][region]
    masked = {k: g for k, g in groups.items() if usable(g) and g['mask_count']}
    if not masked:
        state.clear()
        record['reason'] = 'empty expected seam mask or unknown ownership'
        return
    signature = (evidence_value['id'], evidence_value['mask_version'], tuple(sorted(masked)))
    affected = sum(g['seam_count'] for g in masked.values())
    count = sum(g['mask_count'] for g in masked.values())
    if state.get('signature') != signature or not affected:
        state.clear()
    if affected:
        state.update(signature=signature, consecutive=state.get('consecutive', 0) + 1,
                     start=state.get('start', frame['tick']),
                     start_sub_tick=state.get('start_sub_tick', frame['sub_tick_60']))
    record['samples'] += 1
    record['coverage'] = max(record['coverage'], count / ((current['rects'][region][2] - current['rects'][region][0]) *
                              (current['rects'][region][3] - current['rects'][region][1])))
    event(record, state.get('consecutive', 0), frame, roi, dict(
          consecutive_affected=state.get('consecutive', 0),
          elapsed_ticks=frame['tick'] - state.get('start', frame['tick']) +
          (frame['sub_tick_60'] - state.get('start_sub_tick', frame['sub_tick_60'])) / 60,
          affected_area=affected / count, mean_residual=sum(g['residual'] for g in masked.values()) / count,
          seam_id=evidence_value['id'], mask_version=evidence_value['mask_version']))


def plan_metadata(document):
    """Close unused plan fields too: retained metadata shares the staging budget."""
    plan = document.get('plan')
    scalars = ('span', 'start_tick', 'end_tick', 'ticks_per_day', 'ticks_per_second',
               'dawn_tick', 'dusk_tick', 'strike_tick', 'night_moon_phase_milli', 'tick_limit',
               'frame_limit', 'sparse_interval_ticks', 'presentation_steps', 'max_render_attempts')
    require(isinstance(plan, dict) and set(plan) == set(scalars) | {'boundary_ticks', 'settling', 'frames'},
            'unknown or missing plan metadata')
    require(all(D['integer'](plan[k], 0, (1 << 64) - 1) for k in scalars), 'plan scalar range')
    require(plan['span'] <= 2 and 0 < plan['ticks_per_day'] <= 10000000
            and 0 < plan['ticks_per_second'] <= 1000
            and plan['night_moon_phase_milli'] <= 1000
            and 0 < plan['tick_limit'] <= 10000000 and 0 < plan['frame_limit'] <= 4096,
            'plan limits')
    require(0 < plan['dawn_tick'] < plan['dusk_tick'] < plan['ticks_per_day']
            and plan['start_tick'] <= plan['end_tick']
            and plan['end_tick'] - plan['start_tick'] <= plan['tick_limit'], 'plan tick invariants')
    require((50 * plan['ticks_per_second'] <= plan['strike_tick'] < 90 * plan['ticks_per_second'])
            if plan['span'] == 1 else plan['strike_tick'] == 0, 'plan strike tick')
    require(isinstance(plan['frames'], list) and 0 < len(plan['frames']) <= plan['frame_limit'],
            'plan frame limit')
    boundaries = plan['boundary_ticks']
    require(isinstance(boundaries, list) and len(boundaries) <= 4096
            and all(D['integer'](t, plan['start_tick'], plan['end_tick']) for t in boundaries)
            and all(a < b for a, b in zip(boundaries, boundaries[1:])),
            'plan boundary ticks')
    settling = plan['settling']
    require(isinstance(settling, dict) and set(settling) == {'warmup_frames', 'initial_readiness_retries',
            'sample_settle_frames', 'sample_readiness_retries'}
            and all(D['integer'](n, 0, (1 << 32) - 1) for n in settling.values()), 'plan settling')


def measure(manifest, rois=None):
    path = Path(manifest).absolute()
    root = path.parent.resolve(strict=True)
    # Reuse E1d's closed E1c plan/order/path/hash metadata contract.
    path = root / path.name
    with D['open_unlinked'](root, path) as stream:
        before = os.fstat(stream.fileno())
        document = D['document'](stream, A['MAX_JSON_BYTES'], integer_only=True)
        after = os.fstat(stream.fileno())
        require((before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'manifest changed during read')
        names = []
        with os.scandir(root) as entries:
            for index, entry in enumerate(entries):
                require(index < A['MAX_ENTRIES'], 'directory entry budget')
                if entry.name.startswith('frame-') or Path(entry.name).suffix.lower() in (
                        '.ppm', '.png', '.jpg', '.jpeg'):
                    info = os.lstat(root / entry.name)
                    if (info.st_dev, info.st_ino) == (before.st_dev, before.st_ino):
                        continue
                names.append(entry.name)
    plan_metadata(document)
    ordered = A['sequence_frames'](document, names, '*.diagnostics.json')
    require(document['plan']['end_tick'] - document['plan']['start_tick'] <= 10000000,
            'simulation tick span budget')
    rois = D['load_rois'](rois) if rois is not None else [dict(name='full', rect=[0, 0, 1, 1], metrics=[])]
    require(bool(rois) and len(rois) <= 64, 'nonempty ROI inventory bounded to 64')
    records = [{k: result(k, roi) for k in METRICS} for roi in rois]
    windows, histories, seams = ([{} for _ in rois], [[] for _ in rois], [{} for _ in rois])
    previous, before, seen = None, None, set()
    channel = None
    observed = [dict.fromkeys(CLASSES + ('unsupported',), 0) for _ in rois]
    observed_pixels = [0 for _ in rois]
    for index, (payloads, completion) in enumerate(ordered):
        frame = document['plan']['frames'][index]
        require(sum(p['bytes'] for p in payloads if p['path'].endswith('.json')) <= 1024 * 1024,
                'per-frame JSON budget')
        holder = []
        def consume(meta, capture, table, streams, offsets):
            holder.append(frame_data(meta, capture, table, streams, offsets, rois))
        members = [p['path'] for p in payloads if p['path'].startswith(completion[:-len(D['SUFFIX'])] + '.')]
        D['validate'](root / completion, members, root, consume=consume,
                      payloads={p['path']: p for p in payloads}, seen=seen)
        current = holder[0]
        if previous is not None:
            require(all(current[k] == previous[k] for k in ('width', 'height', 'camera', 'channel')),
                    'changing camera/projection/dimensions/attachment roles')
        channel = current['channel']
        for region, roi in enumerate(rois):
            left, top, right, bottom = current['rects'][region]
            area = (right - left) * (bottom - top)
            observed_pixels[region] += area
            known = 0
            for group in current['groups'][region].values():
                category = group['classification']
                if category in CLASSES:
                    observed[region][category] += group['count']
                    known += group['count']
            observed[region]['unsupported'] += area - known
            if previous is not None:
                adjacent(previous, current, frame, before, roi, region, records[region])
            continuity(histories[region], current, frame, roi, region, records[region]['lighting_continuity'])
            flicker(windows[region], current, frame, roi, region, records[region]['flicker'])
            # Bound changing identities too, not only this frame's producers.
            # At most one extra frame's 1024 tracks exists before this refusal.
            require(sum(len(w.get('tracks', {})) for w in windows) <= MAX_GROUPS,
                    'steady-window track budget')
            seam(seams[region], current, frame, roi, region, records[region]['seam_persistence'])
        previous, before = current, frame
    cadence = sorted({(b['tick'] - a['tick']) * 60 + b['sub_tick_60'] - a['sub_tick_60']
                      for a, b in zip(document['plan']['frames'], document['plan']['frames'][1:])})
    for region in range(len(rois)):
        for record in records[region].values():
            record['observed_coverage'] = {k: v / observed_pixels[region]
                                           for k, v in sorted(observed[region].items())}
    results = [records[i][k] for i in range(len(rois)) for k in METRICS]
    # Eight events per metric across the whole sequence, not eight per ROI.
    for kind in METRICS:
        candidates = sorted((e for r in results if r['metric'] == kind for e in r['events']),
                            key=lambda e: (-e['score'], e['frame_index'], e['roi']))[:8]
        retained = {id(e) for e in candidates}
        for record in results:
            if record['metric'] == kind:
                record['events'] = [e for e in record['events'] if id(e) in retained]
    for record in results:
        if record['status'] == 'advisory':
            record['reason'] = None
        record['worst_frames'] = [dict(index=e['frame_index'], tick=e['tick']) for e in record['events']]
        record['span'] = [document['plan']['start_tick'], document['plan']['end_tick']]
        record['cadence_ticks'] = [n / 60 for n in cadence]
    return dict(schema='luminumbra.temporal_metrics.v1', status='advisory', qualified=False,
                frame_count=len(ordered), channel=channel, identity_resolution='diagnostic producer',
                results=results)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--rois', type=Path)
    args = parser.parse_args(argv)
    try:
        report = measure(args.manifest, args.rois)
    except (OSError, ValueError, RuntimeError, RecursionError, OverflowError) as error:
        print(json.dumps(dict(status='invalid', reason=str(error)), sort_keys=True), file=sys.stderr)
        return 1
    print(json.dumps(report, sort_keys=True, allow_nan=False, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
