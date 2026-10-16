"""Trusted-parent authority, byte identity, waived gates and inherited v1 fields."""
from copy import deepcopy
import struct
import unittest

from meshgen_e4_fixture import arguments, members, glb, EXPORTER
from luminumbra_author.contracts import Refusal, canonical, digest
from luminumbra_author.meshgen_backend import bounded_json
from luminumbra_author.meshgen_config import resolve
from luminumbra_author.meshgen_provenance import build_evidence
from luminumbra_author.meshgen_candidate import validate_evidence


class ParentProvenance(unittest.TestCase):
    def refusal(self, rule, args):
        with self.assertRaises(Refusal) as caught: build_evidence(**args)
        self.assertEqual(caught.exception.finding['rule_id'], rule)

    def test_v2_superset_parent_mock_policy_gpu_unavailable_exact_retained_bytes(self):
        args = arguments(); bundle = build_evidence(**args)
        p = bounded_json(bundle['provenance.json'])
        self.assertEqual(p['finishing'], p['finish'])
        self.assertEqual(p['input']['image_sha256'], p['request']['image_sha256'])
        self.assertEqual(p['raw_output']['sha256'], digest(args['raw_glb']))
        self.assertEqual(p['config']['effective_config_sha256'], digest(bundle['effective_config.json']))
        self.assertEqual(p['licence_gate']['sha256'], digest(bundle['licence_gate.json']))
        self.assertEqual(p['flags']['origin'], 'backend:mock'); self.assertFalse(p['flags']['shippable'])
        for key in ('gpu_name', 'driver', 'cuda', 'torch', 'dtype', 'batch_size', 'peak_vram_mib'):
            self.assertIsNone(p['environment'][key])
        self.assertEqual(validate_evidence(bundle)[0], p)

    def test_policy_forgery_in_worker_observations_and_nested_fields(self):
        for field in ('flags', 'origin', 'shippable', 'waived', 'licence_gate', 'token', 'weights'):
            for nested in (False, True):
                args = arguments()
                if nested: args['observations']['finish']['steps'][0]['stats'][field] = True
                else: args['observations'][field] = True
                with self.subTest(field=field, nested=nested): self.refusal('worker.policy', args)

    def test_mock_lane_refuses_native_looking_worker_observations(self):
        args = arguments()
        args['observations']['finish']['blender_version'] = '5.1.0'
        self.refusal('worker.protocol', args)

    def test_mock_replay_refuses_native_looking_observations_even_when_rehashed(self):
        for simulated in (False, True):
            with self.subTest(simulated=simulated):
                bundle = members()
                provenance = bounded_json(bundle['provenance.json'])
                stats = bounded_json(bundle['stats.json'])
                for finish in (provenance['finish'], provenance['finishing'], stats['observations']):
                    finish['blender_version'] = '5.1.0'
                provenance['environment']['blender_version'] = '5.1.0'
                stats['simulated'] = simulated
                bundle['stats.json'] = canonical(stats)+b'\n'
                provenance['metrics']['stats_sha256'] = digest(bundle['stats.json'])
                bundle['provenance.json'] = canonical(provenance)+b'\n'
                with self.assertRaises(Refusal) as caught: validate_evidence(bundle)
                self.assertEqual(caught.exception.finding['rule_id'],
                                 'worker.protocol' if simulated else 'provenance.inconsistent')

    def test_config_image_raw_result_exporter_and_capability_identity_mismatch(self):
        for mutation, rule in (('image', 'request.invalid'), ('result', 'backend.protocol'), ('exporter', 'worker.protocol'), ('capability', 'backend.pin_mismatch')):
            args = arguments()
            if mutation == 'image': args['image'] += b'x'
            if mutation == 'result': args['result']['sha256'] = 'f'*64
            if mutation == 'exporter': args['observations']['finish']['exporter_sha256'] = 'f'*64
            if mutation == 'capability': args['card']['backend']['adapter_commit'] = 'f'*40
            with self.subTest(mutation=mutation): self.refusal(rule, args)

    def test_prompt_retention_opt_in_and_no_secret_payloads(self):
        args = arguments(); bundle = build_evidence(**args)
        self.assertNotIn('inputs/prompt.txt', bundle)
        self.assertNotIn('prompt_text', bounded_json(bundle['provenance.json'])['request'])
        project = deepcopy(args['resolved'].effective); project['logging']['keep_prompts'] = 'text'
        # Resolving the original committed values avoids treating machine values as local overrides.
        from test_meshgen_config import project_fixture
        project = project_fixture(); project['budgets'] = {'target_height_m': 1}; project['logging'] = {'keep_prompts': 'text'}
        args['resolved'] = resolve(project, 'rock')
        bundle = build_evidence(**args)
        self.assertEqual(bundle['inputs/prompt.txt'], b'')
        validate_evidence(bundle)
        args['token'] = 's'*30+'"\\'; args['observations']['log'] = args['token']
        self.refusal('backend.protocol', args)

    def test_missing_guard_evidence_fails_under_budget_and_accept_flagged_needs_human_plan(self):
        args = arguments(); args['observations']['guard'] = {}
        self.refusal('finish.gate_failed', args)
        from test_meshgen_config import project_fixture
        project = project_fixture(); project['budgets'] = {'target_height_m': 1}
        project['policy']['validation_profile'] = 'internal'
        project['evaluation'] = {'waivable': ['thin_shell_guard']}
        args['resolved'] = resolve(project, 'rock')
        self.refusal('finish.gate_failed', args)
        args['request']['accept_flagged'] = {'human_initiated': True, 'accepted_by': 'reviewer',
            'accepted_at': '2026-10-13T12:00:00Z', 'gates': ['thin_shell_guard']}
        bundle = build_evidence(**args); p = bounded_json(bundle['provenance.json'])
        self.assertEqual(p['flags']['waived'], ['thin_shell_guard']); self.assertFalse(p['flags']['shippable'])
        validate_evidence(bundle)

    def test_worker_step_inventory_bound_and_preview_no_fake_claim(self):
        args = arguments(); args['observations']['finish']['steps'].reverse()
        self.refusal('worker.protocol', args)
        args = arguments(); args['preview'] = b'fake image'
        self.refusal('worker.protocol', args)
        p = bounded_json(members()['stats.json'])
        self.assertTrue(p['simulated'])
        self.assertEqual(p['observations']['tool_versions']['preview'], 'simulated-fixture-v1')

    def test_actual_final_budget_refused_and_grounded_pivot_verified(self):
        args = arguments()
        from test_meshgen_config import project_fixture
        project = project_fixture(); project['budgets'] = {'target_height_m': 1}
        project['profiles']['rock']['budgets']['max_tris_final'] = 64
        args['resolved'] = resolve(project, 'rock')
        data = args['artifact']; size = struct.unpack_from('<I', data, 12)[0]
        doc = bounded_json(data[20:20+size]); blob = data[28+size:]
        doc['nodes'] = [{'mesh': 0} for _ in range(7)]; doc['scenes'][0]['nodes'] = list(range(7))
        args['artifact'] = glb(doc, blob)
        self.refusal('finish.budget_exceeded', args)
        # Triangle count is independent of the backend's tiny raw-output hint.
        args = arguments()
        data = args['artifact']; size = struct.unpack_from('<I', data, 12)[0]
        doc, blob = bounded_json(data[20:20+size]), bytearray(data[28+size:])
        for i in range(36):
            y = struct.unpack_from('<f', blob, i*12+4)[0]; struct.pack_into('<f', blob, i*12+4, y+.25)
        args['artifact'] = glb(doc, bytes(blob))
        self.refusal('finish.gate_failed', args)

    def test_maximum_extent_rejects_centered_export_at_publication_and_replay(self):
        from test_meshgen_config import project_fixture
        project = project_fixture()
        project['budgets'] = {'target_height_m': 1, 'bounds_m': [8, 8, 8], 'max_extent_m': 2}
        project['finish'] = {'scale': {'pivot': 'bbox_center'}}
        args = arguments(); args['resolved'] = resolve(project, 'rock')
        data = args['artifact']; size = struct.unpack_from('<I', data, 12)[0]
        doc = bounded_json(data[20:20+size])
        blob = bytearray(data[28+size:])
        for i in range(36):
            x, y, z = struct.unpack_from('<3f', blob, i*12)
            struct.pack_into('<3f', blob, i*12, 2*x, y-.5, z)
        args['artifact'] = glb(doc, bytes(blob))
        bundle = build_evidence(**args)
        self.assertEqual(validate_evidence(bundle)[1]['artifact']['bounds_m'], [2, 1, 1])

        project['budgets']['max_extent_m'] = 1
        args['resolved'] = resolve(project, 'rock')
        with self.subTest(stage='publication'): self.refusal('finish.gate_failed', args)
        # Rebind a previously passing bundle to the stricter retained config.
        # Replay must recompute the extent gate even when all hashes agree.
        bundle['effective_config.json'] = args['resolved'].stored_bytes
        provenance = bounded_json(bundle['provenance.json'])
        provenance['config'].update(config_digest=args['resolved'].config_digest,
            effective_config_sha256=args['resolved'].stored_sha256, sources=args['resolved'].sources)
        bundle['provenance.json'] = canonical(provenance)+b'\n'
        with self.subTest(stage='replay'):
            with self.assertRaises(Refusal) as caught: validate_evidence(bundle)
            self.assertEqual(caught.exception.finding['rule_id'], 'provenance.inconsistent')
