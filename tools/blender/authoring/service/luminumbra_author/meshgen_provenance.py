"""Trusted-parent provenance v2, retained replay bytes and terminal receipts."""
from copy import deepcopy
import platform

from .contracts import canonical, digest, keys, require
from .meshgen_backend import validate_capability, validate_result
from .meshgen_config import configuration_digest, verify_effective_config, inert_json
from .meshgen_contracts import (PROVENANCE, RECEIPT, accepted_waivers,
                               validate_provenance, validate_receipt, validate_request, validate_backend_params, FINISH, ARTIFACT)
from .meshgen_jobs import inspect_raw_glb, reject_secret
from .meshgen_licence import gate, candidate_eligibility
from .meshgen_metrics import inspect_artifact, guard_decision, validate_png
from .meshgen_registry import validate_schema

POLICY_FIELDS = frozenset(('shippable', 'origin', 'waived', 'licence_gate', 'license', 'eligibility', 'flags', 'token', 'weights'))


def verified_policy_notes(policy, notes):
    """Retain only policy-referenced bytes matching their committed identities."""
    notes = notes or {}
    require(isinstance(notes, dict), 'provenance.inconsistent', 'Malformed policy note evidence.')
    records = [(r['note_id'], r['note_sha256']) for r in policy['attestations'].values() if r['value']]
    records += [(r['counsel_note_id'], r['text_sha256']) for r in policy['counsel_notes']]
    verified = {}
    for identity, expected in records:
        if identity not in notes: continue
        data = notes[identity]
        require(isinstance(data, bytes) and len(data) <= 1024**2 and digest(data) == expected,
                'config.attestation_unverified', 'Policy note bytes differ from the committed hash.')
        verified[identity] = data
    require(notes.keys() == verified.keys(), 'provenance.inconsistent', 'Unregistered policy note evidence.')
    return verified


def validate_observations(value, effective):
    """Workers may supply observations only; policy-shaped fields fail at any depth."""
    inert_json(value, max_depth=effective['limits']['json_depth'], max_items=effective['limits']['collection_items'])
    require(len(canonical(value)) <= effective['limits']['metadata_bytes'], 'worker.limit', 'Worker observations exceed cap.')
    pending = [value]
    while pending:
        item = pending.pop()
        if isinstance(item, dict):
            require(not (POLICY_FIELDS & item.keys()), 'worker.policy', 'Worker cannot assert policy or credentials.')
            pending.extend(item.values())
        elif isinstance(item, list): pending.extend(item)
    keys(value, ('finish', 'guard'), ('log',))
    validate_schema(value['finish'], FINISH)
    # Every currently available execution path is simulated; native finishing
    # remains unavailable, including when replaying retained observations.
    require(value['finish']['blender_version'] == 'simulated',
            'worker.protocol', 'Available finishing requires simulated observations.')
    require([s['id'] for s in value['finish']['steps']] == effective['finish']['steps'],
            'worker.protocol', 'Worker must report each configured step in order.')
    require(isinstance(value['guard'], dict) and isinstance(value.get('log', ''), str)
            and len(value.get('log', '').encode()) <= 16384, 'worker.protocol', 'Malformed observations or log.')
    return value


def build_evidence(*, request, resolved, card, result, image, raw_glb, artifact, preview,
                   observations, plan_digest, definition_digest, document_session,
                   exporter_sha256, backend_log=b'', notes=None, token=None, seed=None):
    """Only this parent function supplies eligibility, waivers and licence authority.

    Inputs are the exact broker snapshots. No backend/worker policy assertion is
    accepted. Return registered member bytes, ready for CandidateStore.publish.
    """
    effective = resolved.effective
    validate_request(request, effective)
    verify_effective_config(resolved.stored_bytes, resolved.stored_sha256, resolved.config_digest)
    validate_observations(observations, effective)
    backend_id = request['backend_id']
    backend = effective['backends'][backend_id]
    capability_digest = validate_capability(card, backend_id, backend)
    params = {**backend.get('params', {}), **effective['backend'].get('params', {}), **request.get('params', {})}
    validate_backend_params(params, card['params_schema'])
    # E4 deliberately preserves E3's disabled real-backend lane.
    require(backend['kind'] == 'mock', 'backend.unavailable', 'Real backend execution is disabled.')
    notes = verified_policy_notes(effective['policy']['licence'], notes)
    licence = gate(card, effective['policy']['licence'], notes)
    require(licence['runnable'], 'licence.status_not_allowed', 'Parent licence admission failed.')
    validate_result(result)
    require(result['sha256'] == digest(raw_glb) and result['bytes'] == len(raw_glb),
            'backend.protocol', 'Result must identify the retained raw bytes.')
    require(digest(image) == request['image_ref']['sha256'], 'request.invalid', 'Retained upload differs from the reviewed image.')
    require(len(image) <= effective['limits']['image_bytes'], 'request.limit', 'Retained image exceeds cap.')
    # Image grammar/credentials were validated by the broker before upload.
    require(image[:8] == b'\x89PNG\r\n\x1a\n' or image[:2] == b'\xff\xd8', 'request.invalid', 'Unsupported retained image.')
    require(len(backend_log) <= 16384, 'backend.limit', 'Backend log exceeds bound.')
    # Preview is an explicitly simulated or native image, validated by the same bounded parser.
    require(preview[:8] == b'\x89PNG\r\n\x1a\n', 'worker.protocol', 'Preview must be PNG.')
    validate_png(preview, effective['limits'])
    require(result['backend_reported']['peak_vram_mib'] is None, 'backend.protocol', 'Mock GPU evidence is unavailable.')
    require(result['backend_reported']['seed'] == (request['seed'] if seed is None else seed), 'backend.protocol', 'Result seed differs.')
    raw_stats = inspect_raw_glb(raw_glb, effective)
    artifact_stats = inspect_artifact(artifact, effective)
    require(artifact_stats['has_normals'], 'finish.gate_failed', 'Export requires nonzero finite normals.')
    guard = guard_decision(observations['guard'], effective['finish']['decimate'])
    gates = quality_gates(artifact_stats, guard, effective)
    failed = [k for k, passed in gates.items() if passed == 'fail']
    waived = accepted_waivers(failed, effective, request.get('accept_flagged'))
    eligibility = candidate_eligibility(licence, backend['kind'], backend_id, waived=waived)
    stats = {'protocol': 'luminumbra.meshgen.stats.v1', 'artifact': artifact_stats, 'guard': guard,
             'gates': gates,
             'observations': deepcopy(observations['finish']), 'guard_observation': deepcopy(observations['guard']),
             'acceptance': deepcopy(request.get('accept_flagged')), 'simulated': True}
    stats_bytes = canonical(stats)+b'\n'
    licence_bytes = canonical(licence)+b'\n'
    finish = deepcopy(observations['finish'])
    require(finish['exporter_sha256'] == exporter_sha256, 'worker.protocol', 'Worker exporter does not match parent pin.')
    shape = next(c for c in card['components'] if c['role'] == 'shape')
    lic = shape['licence']
    prompt = request.get('prompt', '')
    model = {'name': shape['name'], 'repo': shape.get('repo', ''), 'revision': shape['revision'],
             'weights_sha256': shape.get('weights_sha256', []), 'precision': shape.get('precision'),
             'code_commit': card['backend']['adapter_commit'], 'backend_reported': True}
    summary_gate = {k: deepcopy(licence[k]) for k in ('status', 'shippable', 'components', 'dataset_chain_audited', 'ack_ids')}
    summary_gate['sha256'] = digest(licence_bytes)
    provenance = {'schema': PROVENANCE, 'recipe': 'authoring.asset.generate_candidate.v1',
        'definition_sha256': definition_digest, 'plan_sha256': plan_digest, 'request_id': request['request_id'],
        'document_session': document_session, 'model': model,
        'license': {'id': lic['id'], 'text_sha256': lic['text_sha256'], 'url': lic.get('url', ''),
                    'territory_restricted': 'territory_restricted' in lic['clauses'], 'user_acknowledgment': lic['id'] in licence['ack_ids']},
        'input': {'image_sha256': digest(image), 'prompt_sha256': digest(prompt.encode()),
                  'preprocessing_model': None, 'preprocessing_license': None}, 'params': deepcopy(params),
        'raw_output': {'sha256': digest(raw_glb), 'bytes': len(raw_glb), **raw_stats}, 'finishing': deepcopy(finish),
        'config': {'config_digest': resolved.config_digest, 'config_schema_version': 1, 'profile': request['asset_class'],
                   'sources': deepcopy(resolved.sources), 'effective_config_sha256': resolved.stored_sha256},
        'licence_gate': summary_gate,
        'backend': {**card['backend'], 'capability_digest': capability_digest, 'pins_matched': True},
        'environment': {'os': platform.system(), 'blender_version': finish['blender_version'], 'exporter_sha256': exporter_sha256,
                        'python_version': platform.python_version(), **{k: None for k in ('gpu_name', 'driver', 'cuda', 'torch', 'dtype', 'batch_size', 'peak_vram_mib')}},
        'request': {'image_sha256': digest(image), 'declared_source': request['declared_source'], 'prompt_digest': digest(prompt.encode()),
                    'seed': request['seed'] if seed is None else seed, 'params': deepcopy(params),
                    'max_tris_hint': effective['budgets']['max_tris_final'], 'inputs_retained': True},
        'finish': finish, 'metrics': {'stats_sha256': digest(stats_bytes)},
        'artifact': {k: artifact_stats[k] for k in ARTIFACT['properties'] if k != 'sha256'} | {'sha256': digest(artifact)},
        'flags': {**eligibility, 'waived': waived, 'game-repo-only': True, 'no-training-on-outputs': True}}
    if effective['logging']['keep_prompts'] == 'text': provenance['request']['prompt_text'] = prompt
    validate_provenance(provenance)
    members = {'candidate.glb': artifact, 'preview.png': preview, 'provenance.json': canonical(provenance)+b'\n',
        'stats.json': stats_bytes, 'effective_config.json': resolved.stored_bytes, 'licence_gate.json': licence_bytes,
        'inputs/reference_image': image, 'inputs/raw_backend.glb': raw_glb, 'inputs/capabilities.json': canonical(card)+b'\n',
        'inputs/result.json': canonical(result)+b'\n', 'inputs/backend.log': backend_log,
        'inputs/finish.log': observations.get('log', '').encode()}
    if effective['logging']['keep_prompts'] == 'text': members['inputs/prompt.txt'] = prompt.encode()
    if notes:
        # Lossless hex notes share the existing 1 MiB JSON member bound.
        size = 3 + sum(len(canonical(k))+3+2*len(v) for k, v in notes.items()) + len(notes)-1
        require(size <= 1024**2, 'candidate.limit', 'Retained policy notes exceed byte cap.')
        members['inputs/licence_notes.json'] = canonical({k: v.hex() for k, v in notes.items()})+b'\n'
    reject_secret(token, notes, *notes.values())
    reject_secret(token, request, provenance, observations, members, *members.values())
    return members


def make_receipt(provenance, candidate_dir, candidate_sha256, provenance_sha256, stats):
    a, b, gate = provenance['artifact'], provenance['backend'], provenance['licence_gate']
    receipt = {'schema': RECEIPT, 'request_id': provenance['request_id'], 'plan_digest': provenance['plan_sha256'],
        'config_digest': provenance['config']['config_digest'], 'tier': 'backend', 'origin': provenance['flags']['origin'], 'backend': b['id'],
        'licence': {k: gate[k] for k in ('status', 'shippable', 'ack_ids')},
        'candidate': {'dir': candidate_dir, 'sha256': candidate_sha256, 'artifact_sha256': a['sha256'],
            'provenance_sha256': provenance_sha256, 'tris': a['triangles'], **{k: a[k] for k in ('vertices', 'bodies', 'watertight', 'bounds_m', 'budget_ok')}},
        'flags': {'waived': provenance['flags']['waived']}, 'gates': stats['gates'], 'executed': True,
        'spatially_valid': stats['gates']['bounds_within_budgets'] == 'pass', 'timings': {}, 'errors': []}
    validate_receipt(receipt)
    return receipt


def quality_gates(artifact_stats, guard, effective):
    require(artifact_stats['bounds_m'][1] > 0, 'finish.gate_failed', 'Export has zero Y height.')
    target = effective['budgets']['target_height_m']
    pivot = effective['finish']['scale']['pivot']
    low, high = artifact_stats['bounds_min_m'], artifact_stats['bounds_max_m']
    epsilon = max(1e-6, target*1e-6)
    centers = [(a+b)/2 for a, b in zip(low, high)]
    pivot_ok = (abs(low[1]) <= epsilon and abs(centers[0]) <= epsilon and abs(centers[2]) <= epsilon) if pivot == 'base_center' else (
        all(abs(v) <= epsilon for v in centers) if pivot == 'bbox_center' else True)
    gates = {'final_tris_within_budget': artifact_stats['budget_ok'],
             'bounds_within_budgets': all(v <= cap for v, cap in zip(artifact_stats['bounds_m'], effective['budgets']['bounds_m']))
                and max(artifact_stats['bounds_m']) <= effective['budgets']['max_extent_m']
                and abs(artifact_stats['bounds_m'][1]-target) <= epsilon and pivot_ok,
             'thin_shell_guard': guard['pass'] if effective['finish']['decimate']['thin_shell_guard'] else True,
             'uv_present': artifact_stats['has_uv'], 'uv_overlap_zero': artifact_stats['uv_overlap_zero'] is True}
    return {k: 'pass' if v else 'fail' for k, v in gates.items()}
