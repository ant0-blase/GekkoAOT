#!/usr/bin/env python3
"""Build an evidence-based coverage report from a game's renderer log.

GPU grids describe graph outputs, not the whole frame. Presentation is only
proved for matching graph IDs after successful WSI presentation. An unmeasured
field stays null; compiled shaders and enabled options never certify an effect.
"""
import argparse
import collections
import json
from pathlib import Path
import re
import statistics

OUTCOMES = ('NOT_EXECUTED', 'NO_VALID_SCENE', 'NO_CONTRIBUTION',
            'COMPOSITION_LOST', 'VALID_NO_VISIBLE_CHANGE', 'VALID_EFFECT', 'INVALID_OUTPUT')

def fields(line):
    result = dict(re.findall(r'(\w+)=([^\s]+)', line))
    for key, value in result.items():
        if value.isdecimal(): result[key] = int(value)
    return result

def report(lines, game=None, review=None):
    stats = {}; coverage = {}; native = {}; samples = []; presents = []
    cameras = []; invalid = []; sampled_reasons = collections.Counter()
    passes_observed = graphs_observed = 0
    settings = []
    for line in lines:
        if 'GEKKOAOT_ENHANCED_STATS ' in line: stats = fields(line)
        elif 'GEKKOAOT_ENHANCED_COVERAGE ' in line: coverage = fields(line)
        elif 'GEKKOAOT_NATIVE_GX_PROGRESS ' in line: native = fields(line)
        elif 'GEKKOAOT_ENHANCED_MEASUREMENT ' in line:
            sample = fields(line)
            if sample.get('samples'): samples.append(sample)
        elif 'GEKKOAOT_ENHANCED_PRESENT ' in line: presents.append(fields(line))
        elif 'GEKKOAOT_ENHANCED_GRAPH ' in line:
            value = fields(line); graphs_observed = max(graphs_observed, value['graph'])
        elif 'GEKKOAOT_ENHANCED_CONFIG ' in line: settings.append(fields(line))
        elif 'GEKKOAOT_ENHANCED_INVALID_CAMERA ' in line:
            if len(invalid) < 8: invalid.append(fields(line))
        elif 'GEKKOAOT_ENHANCED_PASS ' in line:
            value = fields(line); passes_observed += 1; cameras.append(value['views'])
            for reason, count in re.findall(r'([a-z-]+):(\d+),', value.get('reasons', '')):
                sampled_reasons[reason] += int(count)
    # Never sum sampled per-pass logs as if they were an exhaustive draw census.
    exhaustive_reasons = dict((name, int(count)) for name, count in
                             re.findall(r'([a-z-]+):(\d+),', coverage.get('rejection_counts', '')))
    graph_count = stats.get('graphs', graphs_observed)
    grids = sum(s['samples'] for s in samples)
    valid = sum(s['valid_depth'] for s in samples)
    normal = sum(s['valid_geometric_normal'] for s in samples)
    changed = sum(s['output_changed'] for s in samples)
    shown = {p['graph'] for p in presents if p.get('wsi_success') == 1 and p.get('view') == 0 and p['graph']}
    displayed_samples = [s for s in samples if s['graph'] in shown]
    matched_changed = sum(s['output_changed'] for s in displayed_samples)
    timed = [list(map(int, s['gpu_ns'].split(','))) for s in samples if s.get('timings_valid') == 1]
    timing_names = ('depth', 'ao_and_contact', 'bilateral_x', 'bilateral_y', 'composition')
    groups = collections.defaultdict(list)
    for sample in samples:
        if sample.get('timings_valid') == 1:
            key = tuple(sample.get(k) for k in ('ao_quality','contact_requested','width','height','msaa'))
            groups[key].append(list(map(int, sample['gpu_ns'].split(','))))
    outcome = None; reason = 'Contribution or presentation has not been measured.'
    if not graph_count:
        outcome = 'NO_VALID_SCENE' if stats.get('missing', 0) else 'NOT_EXECUTED'
        reason = 'No enhancement graph completed; inspect camera rejection and configuration.'
    elif samples and not valid:
        outcome = 'NO_VALID_SCENE'; reason = 'The sampled graph outputs contain no valid receivers.'
    elif samples and not changed:
        outcome = 'NO_CONTRIBUTION'; reason = 'Valid sampled receivers produced no measurable output delta.'
    elif matched_changed:
        reason = 'A measured contributing graph was presented; visual correctness still requires review.'
    visual = review or {}
    if visual.get('outcome'):
        if visual['outcome'] not in OUTCOMES or not visual.get('rationale') or not visual.get('captures'):
            raise ValueError('visual review requires a known outcome, rationale and captures')
        if visual['outcome'] == 'VALID_EFFECT' and not matched_changed and not visual.get('same_scene_geometry_ab'):
            raise ValueError('VALID_EFFECT requires presented GPU evidence or a reviewed same-scene geometry A/B')
        outcome = visual['outcome']; reason = visual['rationale']
    return dict(schema=1, game_id=game, outcome=outcome, rationale=reason,
        outcome_values=OUTCOMES, configuration_events=settings, coverage=coverage or None,
        native_submission=native or None, graph_count=graph_count,
        graph_count_scope='total' if 'graphs' in stats else 'lower-bound-from-sampled-log',
        draw_reasons=exhaustive_reasons or None, sampled_pass_reasons=dict(sampled_reasons),
        sampled_pass_count=passes_observed, max_observed_cameras=max(cameras, default=0),
        invalid_camera_examples=invalid, graph_measurement=dict(
            sampled_graphs=len(samples), pixels_sampled=grids,
            valid_depth_fraction=valid/grids if grids else None,
            valid_geometric_normal_fraction=normal/grids if grids else None,
            ao_changed_samples=sum(s['ao_changed'] for s in samples),
            contact_changed_samples=sum(s['contact_changed'] for s in samples),
            output_changed_samples=changed, presented_matching_graphs=len(displayed_samples),
            presented_changed_samples=matched_changed,
            scope='64 by 64 stratified grids of graph outputs; no full-frame coverage claim'),
        gpu_timing_ns={name: dict(samples=len(timed), median=statistics.median(row[i] for row in timed))
                       for i, name in enumerate(timing_names)} if timed else None,
        gpu_timing_by_configuration=[dict(zip(('ao_quality','contact_requested','width','height','msaa'),key),
            samples=len(rows),median_ns={name:statistics.median(row[i] for row in rows)
                for i,name in enumerate(timing_names)}) for key,rows in groups.items()],
        gpu_timing_scope='AO and contact share one pass; independent AO/contact runs are needed to separate cost',
        effects_unsupported=('world_sun', 'csm_composition', 'ssgi', 'ssr'), visual_review=review,
        limitations=['Unmatched graph IDs do not prove composition was lost.',
                     'Alias content is not certified by the direct-presentation counter.',
                     'A visible pixel delta alone does not certify correct lighting.'])

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path); p.add_argument('--game-id'); p.add_argument('--review', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    with args.log.open(errors='replace') as source:
        result = report(source, args.game_id, json.loads(args.review.read_text()) if args.review else None)
    text = json.dumps(result, indent=2) + '\n'
    if args.output: args.output.write_text(text)
    else: print(text, end='')

if __name__ == '__main__': main()
