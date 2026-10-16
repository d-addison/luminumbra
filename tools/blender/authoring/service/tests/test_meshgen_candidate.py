"""Seal/receipt crash ordering, held-reader publication and append-only evidence."""
from copy import deepcopy
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
import os
import stat
import struct
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from meshgen_e4_fixture import arguments, glb, members
from luminumbra_author.contracts import Refusal, canonical, digest
from luminumbra_author.meshgen_backend import bounded_json
from luminumbra_author.meshgen_candidate import CandidateStore, read_member
from luminumbra_author.meshgen_config import resolve
from luminumbra_author.meshgen_provenance import build_evidence
from test_meshgen_config import project_fixture
from luminumbra_author import file_io


JOB = digest(b'job')[:32]
FIRST = digest(b'first')[:32]
SECOND = digest(b'second')[:32]


class CandidateStorage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.project = Path(self.tmp.name)
        self.store = CandidateStore(self.project)
        self.members = members()

    def refusal(self, rule, call):
        with self.assertRaises(Refusal) as caught: call()
        self.assertEqual(caught.exception.finding['rule_id'], rule)

    def test_complete_seal_retains_replay_distinct_identities_and_receipt(self):
        receipt = self.store.publish(JOB, self.members)
        bundle = self.store.read(JOB)
        self.assertNotEqual(bundle['sha256'], digest(self.members['candidate.glb']))
        self.assertEqual(receipt, self.store.receipt(JOB))
        self.assertEqual(bundle['members'], self.members)
        candidate, area = self.store.paths(JOB)
        self.assertFalse((candidate/'receipt.json').exists())
        self.assertTrue((area/'receipt.prepared.json').exists())
        self.refusal('request.conflict', lambda: self.store.publish(JOB, self.members))

    def test_seal_rechecks_scene_selection_in_artifact_and_raw_evidence(self):
        for member in ('candidate.glb', 'inputs/raw_backend.glb'):
            for count, default, rule in ((1, 'absent', None), (2, 'absent', 'scene.ambiguous'),
                                         (2, 0, None), (2, 1, None), (2, 2, 'glb.bounds')):
                with self.subTest(member=member, count=count, default=default):
                    evidence = dict(self.members)
                    data = evidence[member]; size = struct.unpack_from('<I', data, 12)[0]
                    doc = bounded_json(data[20:20+size])
                    doc['scenes'] = [{} for _ in range(count)]
                    doc['scenes'][default if type(default) is int and 0 <= default < count else 0] = {'nodes': [0]}
                    if default == 'absent': doc.pop('scene')
                    else: doc['scene'] = default
                    evidence[member] = glb(doc, data[28+size:])
                    provenance = bounded_json(evidence['provenance.json'])
                    field = 'artifact' if member == 'candidate.glb' else 'raw_output'
                    provenance[field]['sha256'] = digest(evidence[member])
                    if field == 'raw_output':
                        provenance[field]['bytes'] = len(evidence[member])
                        result = bounded_json(evidence['inputs/result.json'])
                        result.update(sha256=digest(evidence[member]), bytes=len(evidence[member]))
                        evidence['inputs/result.json'] = canonical(result)+b'\n'
                    evidence['provenance.json'] = canonical(provenance)+b'\n'
                    job = digest(canonical([member, count, default]))[:32]
                    candidate, area = self.store.paths(job)
                    if rule:
                        self.refusal(rule, lambda: self.store.publish(job, evidence))
                        self.assertFalse(candidate.exists())
                        self.assertFalse((area/'receipt.prepared.json').exists())
                    else:
                        receipt = self.store.publish(job, evidence)
                        self.assertEqual(self.store.read(job)['members'], evidence)
                        self.assertEqual(self.store.receipt(job), receipt)

    def test_member_tamper_missing_extra_and_manifest_rewrite(self):
        for mutation, rule in (('tamper', 'candidate.changed'), ('missing', 'candidate.members'), ('extra', 'candidate.members'), ('manifest', 'candidate.changed')):
            with self.subTest(mutation=mutation):
                job = digest(mutation.encode())[:32]
                self.store.publish(job, self.members)
                candidate, _ = self.store.paths(job)
                if mutation == 'tamper': (candidate/'candidate.glb').write_bytes(b'bad')
                if mutation == 'missing': (candidate/'preview.png').unlink()
                if mutation == 'extra': (candidate/'policy.json').write_bytes(b'{}')
                if mutation == 'manifest': (candidate/'manifest.json').write_bytes(b'{}')
                self.refusal(rule, lambda: self.store.read(job))

    def test_escapes_symlinks_and_hardlinks_refused(self):
        for job in ('../escape', '/absolute', 'a:b', 'a/b'):
            self.refusal('candidate.path', lambda: self.store.read(job))
        self.store.publish(JOB, self.members)
        candidate, _ = self.store.paths(JOB)
        target = self.project/'outside'; target.write_bytes(self.members['preview.png'])
        (candidate/'preview.png').unlink()
        try: (candidate/'preview.png').symlink_to(target)
        except OSError as error:
            if os.name != 'nt' or getattr(error, 'winerror', None) != 1314: raise
            # Windows CI without symlink privileges still proves hard-link refusal.
            os.link(target, candidate/'preview.png')
        self.refusal('candidate.path', lambda: self.store.read(JOB))
        (candidate/'preview.png').unlink(); os.link(target, candidate/'preview.png')
        self.refusal('candidate.path', lambda: self.store.read(JOB))

    def test_walk_uses_complete_metadata_when_directory_entries_have_zero_link_count(self):
        self.store.publish(JOB, self.members)
        candidate, _ = self.store.paths(JOB)
        scandir = os.scandir

        @contextmanager
        def windows_scandir(directory):
            with scandir(directory) as entries:
                wrapped = []
                for entry in entries:
                    info = entry.stat(follow_symlinks=False)
                    fields = {name: getattr(info, name) for name in dir(info) if name.startswith('st_')}
                    fields.update(st_ino=0, st_dev=0, st_nlink=0)
                    incomplete = SimpleNamespace(**fields)
                    wrapped.append(SimpleNamespace(name=entry.name, path=entry.path,
                        stat=lambda *, follow_symlinks=True, info=incomplete: info))
                yield wrapped

        with patch('luminumbra_author.meshgen_candidate.os.scandir', windows_scandir):
            with os.scandir(candidate) as entries:
                entry = next(entry for entry in entries if entry.name == 'preview.png')
                self.assertEqual(entry.stat(follow_symlinks=False).st_nlink, 0)
                self.assertEqual(os.lstat(entry.path).st_nlink, 1)
            self.assertEqual(self.store._walk(candidate), set(self.members) | {'manifest.json', 'candidate.sha256'})
            self.assertEqual(self.store.read(JOB)['members'], self.members)
            target = self.project/'outside'; target.write_bytes(self.members['preview.png'])
            (candidate/'preview.png').unlink(); os.link(target, candidate/'preview.png')
            self.assertEqual(os.lstat(candidate/'preview.png').st_nlink, 2)
            self.refusal('candidate.path', lambda: self.store._walk(candidate))
            self.refusal('candidate.path', lambda: self.store.read(JOB))

    def test_seal_and_receipt_order_recovery_after_each_publication_interruption(self):
        total = 1+len(self.members)+1+1+2
        for stop_at in range(1, total+1):
            with self.subTest(stop_at=stop_at):
                store = CandidateStore(self.project, 'd'+str(stop_at))
                actual, writes = store._write, []
                def fail_after(path, data, decoded=None):
                    actual(path, data, decoded); writes.append(path.name)
                    if len(writes) == stop_at: raise OSError('injected post-publication interruption')
                with patch.object(store, '_write', side_effect=fail_after):
                    with self.assertRaises(OSError): store.publish(JOB, self.members)
                candidate, area = store.paths(JOB)
                if (candidate/'candidate.sha256').exists():
                    recovered = store.recover(JOB)
                    self.assertEqual(recovered, store.receipt(JOB))
                    self.assertEqual(writes[:1], ['receipt.prepared.json'])
                else:
                    self.refusal('candidate.incomplete', lambda: store.recover(JOB))
                    self.assertFalse((area/'receipt.sha256').exists())

    def test_recovery_refuses_changed_prepared_and_terminal_receipt(self):
        self.store.publish(JOB, self.members)
        _, area = self.store.paths(JOB)
        data = bounded_json((area/'receipt.prepared.json').read_bytes()); data['plan_digest'] = 'f'*64
        (area/'receipt.prepared.json').write_bytes(canonical(data)+b'\n')
        self.refusal('receipt.changed', lambda: self.store.recover(JOB))
        (area/'receipt.json').write_bytes(b'{}')
        self.refusal('receipt.changed', lambda: self.store.receipt(JOB))

    def test_secret_exclusion_decoded_json_glb_logs_and_rotation(self):
        token = 'a'*30+'"\\'
        for name, payload in (('inputs/backend.log', token.encode()), ('inputs/result.json', canonical({'escaped': token})),
                              ('provenance.json', canonical({'escaped': token}))):
            with self.subTest(name=name):
                bad = dict(self.members); bad[name] = payload
                store = CandidateStore(self.project, tokens=[token])
                self.refusal('backend.protocol', lambda: store.publish(JOB, bad))
                self.assertFalse(store.paths(JOB)[0].exists())
        self.store.publish(JOB, self.members)
        store = CandidateStore(self.project, tokens=['rotated-token-value-which-is-long-enough'])
        self.refusal('backend.protocol', lambda: store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {'log': store.tokens[0]}))

    def test_parent_policy_forgery_rejected_even_with_rehashed_members(self):
        bad = dict(self.members)
        p = bounded_json(bad['provenance.json']); p['flags']['shippable'] = True
        bad['provenance.json'] = canonical(p)+b'\n'
        self.refusal('provenance.mock', lambda: self.store.publish(JOB, bad))
        bad = dict(self.members); gate = bounded_json(bad['licence_gate.json']); gate['ack_ids'] = ['fabricated']
        bad['licence_gate.json'] = canonical(gate)+b'\n'
        self.refusal('provenance.inconsistent', lambda: self.store.publish(JOB, bad))

    def test_configured_retention_caps_apply_to_import_and_attestation(self):
        scandir = os.scandir
        for cap, used_mib in ((64, 64), (8192, 4097)):
            for operation in ('import', 'attestation'):
                with self.subTest(cap=cap, operation=operation):
                    store = CandidateStore(self.project, operation+str(cap))
                    project = project_fixture(); project['budgets'] = {'target_height_m': 1}
                    project['retention'] = {'max_total_mib': cap}
                    args = arguments(); args['resolved'] = resolve(project, 'rock')
                    receipt = store.publish(JOB, build_evidence(**args))
                    candidate, area = store.paths(JOB)
                    retained = store.root/'retained.bin'; retained.write_bytes(b'x')

                    @contextmanager
                    def retained_usage(directory):
                        with scandir(directory) as entries:
                            wrapped = []
                            for entry in entries:
                                if Path(entry.path) == retained:
                                    info = entry.stat(follow_symlinks=False)
                                    fields = {name: getattr(info, name) for name in dir(info) if name.startswith('st_')}
                                    fields['st_size'] = used_mib*1024**2
                                    entry = SimpleNamespace(name=entry.name, path=entry.path,
                                        stat=lambda *, follow_symlinks=True, info=SimpleNamespace(**fields): info)
                                wrapped.append(entry)
                            yield wrapped

                    destination = area/'import-private'/'snapshot.glb'
                    def mutate():
                        if operation == 'import':
                            return store.stage_for_import(JOB, receipt['candidate']['sha256'],
                                receipt['candidate']['provenance_sha256'], destination)
                        return store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {'score': 1})

                    head = store._head_path(JOB).read_bytes()
                    # Account for large retained usage without allocating large files.
                    with patch('luminumbra_author.meshgen_candidate.os.scandir', retained_usage):
                        if cap == 64: self.refusal('candidate.limit', mutate)
                        else: mutate()
                    if cap == 64:
                        self.assertEqual(store._head_path(JOB).read_bytes(), head)
                        self.assertFalse(destination.exists())
                        self.assertFalse((area/'attestations.json').exists())
                        self.assertFalse((candidate/'attestations').exists())
                    elif operation == 'import':
                        self.assertEqual(destination.read_bytes(), self.members['candidate.glb'])
                    else:
                        self.assertEqual(store.attestations(JOB)[0]['observation'], {'score': 1})
                    retained.unlink()

    def test_project_quota_serializes_imports_and_attestations_across_documents(self):
        cap = 64*1024**2
        for operation in ('import', 'attestation'):
            with self.subTest(operation=operation):
                stores = [CandidateStore(self.project, operation+suffix) for suffix in ('_first', '_second')]
                project = project_fixture(); project['budgets'] = {'target_height_m': 1}
                project['retention'] = {'max_total_mib': 64}
                args = arguments(); args['resolved'] = resolve(project, 'rock')
                evidence = build_evidence(**args)
                receipts = [store.publish(JOB, evidence) for store in stores]
                destinations = [store.paths(JOB)[1]/'import-private'/'snapshot.glb' for store in stores]
                payload = {'detail': 'x'*(64*1024)}
                remaining = (len(evidence['candidate.glb'])+1152 if operation == 'import'
                             else 11*len(canonical(payload))//2)
                state = stores[0].root.parent
                retained = state/'retained.bin'
                used = sum(p.stat().st_size for p in state.rglob('*') if p.is_file())
                # Sparse filler leaves room for one operation, but not both.
                with retained.open('wb') as stream: stream.truncate(cap-used-remaining)

                def mutate(index):
                    if operation == 'import':
                        receipt = receipts[index]['candidate']
                        return stores[index].stage_for_import(JOB, receipt['sha256'],
                            receipt['provenance_sha256'], destinations[index])
                    return stores[index].append_attestation(JOB, 'metrics', '20261013T123456000001Z', payload)

                admitted, resume = threading.Event(), threading.Event()
                quota = stores[0]._quota
                def hold_after_admission(incoming, byte_limit):
                    quota(incoming, byte_limit)
                    admitted.set()
                    self.assertTrue(resume.wait(5), 'Publication was not released.')

                try:
                    with patch.object(stores[0], '_quota', side_effect=hold_after_admission):
                        with ThreadPoolExecutor(max_workers=1) as pool:
                            first = pool.submit(mutate, 0)
                            try:
                                self.assertTrue(admitted.wait(5), 'First document did not reach quota admission.')
                                self.refusal('busy', lambda: mutate(1))
                            finally:
                                resume.set()
                            first.result(timeout=5)
                    self.refusal('candidate.limit', lambda: mutate(1))
                    if operation == 'import':
                        self.assertEqual(destinations[0].read_bytes(), evidence['candidate.glb'])
                        self.assertFalse(destinations[1].exists())
                    else:
                        self.assertEqual(stores[0].attestations(JOB)[0]['observation'], payload)
                        self.assertEqual(stores[1].attestations(JOB), [])
                    self.assertLessEqual(sum(p.stat().st_size for p in state.rglob('*') if p.is_file()), cap)
                finally:
                    retained.unlink()

    def test_attestation_peak_quota_refuses_before_pointer_and_allows_smaller_append(self):
        cap = 64*1024**2
        payload = {'detail': 'x'*(600*1024)}
        for copies in (3.5, 4.5):
            with self.subTest(copies=copies):
                store = CandidateStore(self.project, 'peak_'+str(int(copies*10)))
                project = project_fixture(); project['budgets'] = {'target_height_m': 1}
                project['retention'] = {'max_total_mib': 64}
                args = arguments(); args['resolved'] = resolve(project, 'rock')
                store.publish(JOB, build_evidence(**args))
                remaining = int(copies*len(canonical(payload)))
                # Sparse filler charges real file sizes without large allocation.
                retained = store.root/'retained.bin'
                used = sum(p.stat().st_size for p in store.root.parent.rglob('*') if p.is_file())
                with retained.open('wb') as stream: stream.truncate(cap-used-remaining)
                def snapshot():
                    return {p.relative_to(store.root): p.read_bytes() if p.is_file() else None
                            for p in store.root.rglob('*') if p != retained}
                before = snapshot()
                try:
                    self.refusal('candidate.limit', lambda: store.append_attestation(
                        JOB, 'metrics', '20261013T123456000001Z', payload))
                    self.assertEqual(snapshot(), before)
                    fresh = CandidateStore(self.project, store.root.name)
                    self.assertEqual(fresh.recover_attestation(JOB), [])
                    result = fresh.append_attestation(JOB, 'metrics', '20261013T123456000002Z', {'score': 1})
                    self.assertEqual([entry['observation'] for entry in result], [{'score': 1}])
                    self.assertLessEqual(sum(p.stat().st_size for p in store.root.parent.rglob('*') if p.is_file()), cap)
                finally:
                    retained.unlink()

    def test_attested_policy_publishes_retains_replay_notes_and_refuses_tampering(self):
        note_bytes = b'Human reviewed revenue evidence.\n'
        note = self.project/'authoring/meshgen/notes/revenue.md'
        note.parent.mkdir(parents=True); note.write_bytes(note_bytes)
        counsel_bytes = b'Archived counsel evidence.\r\n\xff'
        counsel = note.with_name('counsel.md'); counsel.write_bytes(counsel_bytes)
        project = project_fixture(); project['budgets'] = {'target_height_m': 1}
        project['policy']['licence'] = {'attestations': {'revenue_under_cap': {
            'value': True, 'attested_by': 'reviewer', 'attested_at': '2026-10-13',
            'note_id': 'revenue', 'note_sha256': digest(note_bytes)}},
            'counsel_notes': [{'counsel_note_id': 'counsel', 'covers_licence_ids': ['MIT'],
                'text_sha256': digest(counsel_bytes), 'recorded_by': 'reviewer',
                'recorded_at': '2026-10-13', 'outcome': 'cleared_internal'}]}
        args = arguments(); args['resolved'] = resolve(project, 'rock', project_root=self.project)
        args['notes'] = {'revenue': note_bytes, 'counsel': counsel_bytes}
        evidence = build_evidence(**args)
        self.refusal('config.attestation_unverified', lambda: build_evidence(
            **{**args, 'notes': {'revenue': b'changed'}}))
        receipt = self.store.publish(JOB, evidence)
        note.unlink(); counsel.unlink()
        fresh = CandidateStore(self.project)
        self.assertEqual(fresh.read(JOB)['members'], evidence)
        self.assertEqual(fresh.receipt(JOB), receipt)
        self.assertEqual({k: bytes.fromhex(v) for k, v in bounded_json(evidence['inputs/licence_notes.json']).items()}, args['notes'])
        bad = dict(evidence)
        bad['inputs/licence_notes.json'] = canonical({'revenue': b'changed'.hex()})+b'\n'
        self.refusal('config.attestation_unverified', lambda: self.store.publish(SECOND, bad))
        self.assertFalse(self.store.paths(SECOND)[0].exists())
        candidate, _ = self.store.paths(JOB)
        (candidate/'inputs/licence_notes.json').write_bytes(bad['inputs/licence_notes.json'])
        self.refusal('candidate.changed', lambda: fresh.read(JOB))

    def test_oversize_attestation_anchor_refuses_without_durable_change(self):
        self.store.publish(JOB, self.members)
        payload = {'detail': 'x'*(600*1024)}
        self.store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', payload)
        def snapshot():
            return {p.relative_to(self.store.root): digest(p.read_bytes())
                    for p in self.store.root.rglob('*') if p.is_file()}
        before = snapshot()
        self.refusal('candidate.limit', lambda: self.store.append_attestation(
            JOB, 'metrics', '20261013T123456000002Z', payload))
        self.assertEqual(snapshot(), before)
        fresh = CandidateStore(self.project)
        self.assertEqual(len(fresh.recover_attestation(JOB)), 1)
        result = fresh.append_attestation(JOB, 'metrics', '20261013T123456000003Z', {'score': 1})
        self.assertEqual(len(result), 2)
        self.assertEqual(result[-1]['observation'], {'score': 1})

    def test_attestation_record_nesting_refuses_before_pending_pointer(self):
        self.store.publish(JOB, self.members)
        payload = {}
        for _ in range(30): payload = {'nested': payload}
        self.assertEqual(bounded_json(canonical(payload)), payload)
        head = self.store._head_path(JOB).read_bytes()
        self.refusal('backend.limit', lambda: self.store.append_attestation(
            JOB, 'metrics', '20261013T123456000001Z', payload))
        candidate, area = self.store.paths(JOB)
        self.assertEqual(self.store._head_path(JOB).read_bytes(), head)
        self.assertFalse((area/'attestations.json').exists())
        self.assertFalse((area/'attestation-history').exists())
        self.assertFalse((candidate/'attestations').exists())
        self.assertEqual(len(self.store.append_attestation(
            JOB, 'metrics', '20261013T123456000002Z', {'score': 1})), 1)

    def test_append_only_attestations_preserve_original_seal_detect_rewrite_truncation(self):
        receipt = self.store.publish(JOB, self.members)
        before = self.store.read(JOB)['sha256']
        self.store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {'score': 1})
        self.store.append_attestation(JOB, 'judge', '20261013T123456000002Z', {'advisory': True})
        self.assertEqual(before, self.store.read(JOB)['sha256'])
        self.assertEqual(receipt, self.store.receipt(JOB))
        self.refusal('attestation.changed', lambda: self.store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {}))
        candidate, _ = self.store.paths(JOB)
        path = candidate/'attestations'/'metrics.20261013T123456000001Z.json'
        old = path.read_bytes(); path.write_bytes(b'{}')
        self.refusal('attestation.changed', lambda: CandidateStore(self.project).read(JOB))
        path.write_bytes(old); path.unlink()
        self.refusal('attestation.changed', lambda: CandidateStore(self.project).read(JOB))

    def test_attestation_interrupted_append_recovery(self):
        for stop_at in (1, 2, 3, 4, 5):
            store = CandidateStore(self.project, 'd'+str(stop_at)); store.publish(JOB, self.members)
            actual, count = store._write, [0]
            def fail(path, data, decoded=None):
                actual(path, data, decoded); count[0] += 1
                if count[0] == stop_at: raise OSError('interrupted append')
            with patch.object(store, '_write', side_effect=fail):
                with self.assertRaises(OSError): store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {'score': 1})
            self.assertEqual(len(store.attestations(JOB)), 1 if stop_at == 5 else 0)
            self.assertEqual(len(store.recover_attestation(JOB)), 1)
            self.assertEqual(len(store.attestations(JOB)), 1)

    def test_held_shared_readers_and_durability_use_shared_publication(self):
        self.store.publish(JOB, self.members)
        _, area = self.store.paths(JOB)
        path = area/'receipt.json'; original = path.read_bytes()
        with file_io.open_regular_reader(path) as reader:
            self.store._write(path, original, bounded_json(original))
            self.assertEqual(reader.read(), original)
            self.assertEqual(path.read_bytes(), original)
        self.refusal('candidate.retained', lambda: self.store.delete(JOB))

    def test_cancellation_and_commit_share_e3_owner_transition(self):
        from luminumbra_author.meshgen_jobs import RequestLedger
        ledger = RequestLedger(self.project)
        record = ledger.begin('generate:e4', digest(b'plan'))
        job = record['job_id']; ledger.update(job, state='finishing')
        cancelled = ledger.cancel(job)
        with patch.object(self.store, 'publish') as publish:
            self.assertEqual(ledger.commit_success(job, lambda: self.store.publish(job, self.members)), cancelled)
            publish.assert_not_called()
        self.assertFalse(self.store.paths(job)[0].exists())

    def test_publication_binds_backend_and_config_to_ledger_owner(self):
        from luminumbra_author.meshgen_candidate import CandidateLedger
        provenance = bounded_json(self.members['provenance.json'])
        effective = bounded_json(self.members['effective_config.json'])
        backend = provenance['backend']['id']
        for changed in (None, 'config_digest', 'backend_id'):
            with self.subTest(changed=changed):
                document = changed or 'matching'
                ledger = CandidateLedger(self.project, document=document)
                store = CandidateStore(self.project, document)
                context = {'profile': provenance['config']['profile'], 'backend_id': backend,
                    'backend_endpoint': effective['backends'][backend]['endpoint'],
                    'config_digest': provenance['config']['config_digest'], 'overrides': None}
                if changed == 'config_digest': context[changed] = digest(b'other config')
                elif changed == 'backend_id': context[changed] = 'other_backend'
                record = ledger.begin(provenance['request_id'], provenance['plan_sha256'], backend_context=context)
                job = record['job_id']
                ledger.update(job, state='finishing', task_drained=True, backend_stopped=True)
                if changed is None:
                    terminal = ledger.commit_success(job, lambda: store.publish(job, self.members))
                    self.assertEqual(terminal['state'], 'succeeded')
                    self.assertEqual(terminal['result'], store.receipt(job))
                else:
                    self.refusal('receipt.changed', lambda: ledger.commit_success(job, lambda: store.publish(job, self.members)))
                    candidate, area = store.paths(job)
                    self.assertFalse(candidate.exists())
                    self.assertFalse((area/'receipt.prepared.json').exists())
                    self.assertEqual(ledger.status(job)['state'], 'finishing')
                    ledger.cancel(job)

    def test_committed_seal_wins_cancel_and_recovers_original_e3_terminal_replay(self):
        from luminumbra_author.meshgen_candidate import CandidateLedger
        ledger = CandidateLedger(self.project, owner_start=lambda pid: 'fixture-owner')
        record = ledger.begin('generate:e4', digest(b'plan')); job = record['job_id']
        ledger.update(job, state='finishing', task_drained=True, backend_stopped=True)
        # Crash after candidate commit but before E3 terminal success publication.
        self.store.publish(job, self.members)
        recovered = ledger.cancel(job)
        self.assertEqual(recovered['state'], 'succeeded')
        self.assertEqual(recovered['result'], self.store.receipt(job))
        self.assertEqual(ledger.begin('generate:e4', digest(b'plan')), recovered)

    def test_orphan_recovery_uses_seal_prepared_receipt_and_refuses_policy_tampering(self):
        from luminumbra_author.meshgen_candidate import CandidateLedger
        for damaged in (False, True):
            with self.subTest(damaged=damaged):
                document = 'orphan'+str(damaged)
                ledger = CandidateLedger(self.project, document=document, owner_start=lambda pid: 'fixture-owner')
                store = CandidateStore(self.project, document)
                record = ledger.begin('generate:e4', digest(b'plan')); job = record['job_id']
                ledger.update(job, state='finishing', task_drained=True, backend_stopped=True)
                store.publish(job, self.members)
                if damaged: (store.paths(job)[0]/'candidate.glb').write_bytes(b'damaged')
                ledger.owner_start = lambda pid: None
                if damaged:
                    self.refusal('candidate.changed', ledger.recover)
                    self.assertTrue((self.project/'.luminumbra-author-recipes'/'.meshgen.lock').exists())
                else:
                    ledger.recover()
                    self.assertEqual(ledger.result(job)['state'], 'succeeded')
                    self.assertEqual(ledger.begin('generate:e4', digest(b'plan'))['result'], store.receipt(job))

    def test_dynamic_token_rotation_refuses_document_before_state_and_attestation_before_anchor(self):
        credential = ['q'*32]
        self.refusal('backend.protocol', lambda: CandidateStore(self.project, credential[0], secret_source=lambda: credential))
        self.assertFalse((self.project/'.luminumbra-author-recipes').exists())
        store = CandidateStore(self.project, secret_source=lambda: credential)
        store.publish(JOB, self.members)
        credential[0] = 'r'*32
        self.refusal('backend.protocol', lambda: store.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {'value': credential[0]}))
        self.assertFalse((store.paths(JOB)[1]/'attestations.json').exists())
        # A rotated credential can match a formerly harmless document name.
        # Pointer preparation must apply the same path-secret check as _write.
        document = 's'*32
        rotated = CandidateStore(self.project, document, secret_source=lambda: credential)
        rotated.publish(JOB, self.members)
        pointer = rotated._head_path(JOB); before = pointer.read_bytes()
        credential[0] = document
        self.refusal('backend.protocol', lambda: rotated.append_attestation(JOB, 'metrics', '20261013T123456000001Z', {}))
        self.assertEqual(pointer.read_bytes(), before)
        self.assertFalse((rotated.paths(JOB)[1]/'attestations.json').exists())

    def test_project_byte_quota_refuses_publication_preserving_last_receipt(self):
        self.store.publish(FIRST, self.members)
        first = self.store.receipt(FIRST)
        with patch.object(self.store, '_quota', side_effect=Refusal('candidate.limit', 'full')):
            self.refusal('candidate.limit', lambda: self.store.publish(SECOND, self.members))
        self.assertEqual(self.store.receipt(FIRST), first)
        self.assertFalse(self.store.paths(SECOND)[0].exists())

    def test_subprocess_crash_at_candidate_marker_recovers_seal_and_terminal_success(self):
        import subprocess
        import sys
        service = Path(__file__).resolve().parents[1]
        tests = Path(__file__).resolve().parent
        source = """
import os, sys
from pathlib import Path
from meshgen_e4_fixture import members
from luminumbra_author.contracts import digest
from luminumbra_author.meshgen_candidate import CandidateLedger, CandidateStore
project = Path(sys.argv[1])
ledger = CandidateLedger(project)
record = ledger.begin('generate:e4', digest(b'plan'))
job = record['job_id']
ledger.update(job, state='finishing', task_drained=True, backend_stopped=True)
print(job, flush=True)
store = CandidateStore(project)
write = store._write
def crash(path, data, decoded=None):
    write(path, data, decoded)
    if path.name == 'candidate.sha256': os._exit(23)
store._write = crash
ledger.commit_success(job, lambda: store.publish(job, members()))
"""
        bootstrap = 'import sys; sys.path[:0]='+repr([str(service), str(tests)])+'; exec('+repr(source)+')'
        completed = subprocess.run([sys.executable, '-I', '-S', '-B', '-c', bootstrap, str(self.project)],
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 23, completed.stderr)
        job = completed.stdout.strip()
        from luminumbra_author.meshgen_candidate import CandidateLedger
        ledger = CandidateLedger(self.project)
        ledger.recover()
        terminal = ledger.begin('generate:e4', digest(b'plan'))
        self.assertEqual(terminal['state'], 'succeeded')
        self.assertEqual(terminal['result'], self.store.receipt(job))

    def test_regular_publication_fsyncs_before_shared_replace_and_posix_directory_after(self):
        self.store.publish(JOB, self.members)
        candidate, _ = self.store.paths(JOB)
        events = []
        replace, fsync = file_io.replace_file, os.fsync
        def record_replace(src, dst, **kwargs):
            events.append(('replace', kwargs)); return replace(src, dst, **kwargs)
        def record_fsync(fd): events.append(('fsync', None)); return fsync(fd)
        with patch.object(file_io, 'replace_file', side_effect=record_replace), patch.object(os, 'fsync', side_effect=record_fsync):
            self.store._write(candidate/'preview.png', self.members['preview.png'])
        self.assertEqual(events[0][0], 'fsync')
        self.assertEqual(events[1][0], 'replace')
        self.assertTrue(events[1][1]['durable'])
        if os.name != 'nt':
            self.assertIn('source_dir_fd', events[1][1])
            self.assertEqual(events[2][0], 'fsync')
        else: self.assertTrue(events[1][1]['allow_cross_directory'])

    def test_forged_gate_stats_and_prepared_owner_plan_refused(self):
        bad = dict(self.members)
        stats = bounded_json(bad['stats.json']); stats['gates']['uv_present'] = 'fail'
        bad['stats.json'] = canonical(stats)+b'\n'
        p = bounded_json(bad['provenance.json']); p['metrics']['stats_sha256'] = digest(bad['stats.json'])
        bad['provenance.json'] = canonical(p)+b'\n'
        self.refusal('provenance.inconsistent', lambda: self.store.publish(JOB, bad))
        from luminumbra_author.meshgen_candidate import CandidateLedger
        ledger = CandidateLedger(self.project)
        record = ledger.begin('generate:e4', digest(b'different plan')); job = record['job_id']
        ledger.update(job, state='finishing')
        self.refusal('receipt.changed', lambda: self.store.publish(job, self.members))
        self.assertFalse((self.store.paths(job)[0]/'candidate.sha256').exists())
        self.assertTrue((self.project/'.luminumbra-author-recipes'/'.meshgen.lock').exists())

    def test_import_private_snapshot_uses_verified_bytes_despite_source_replacement(self):
        receipt = self.store.publish(JOB, self.members)
        candidate, area = self.store.paths(JOB)
        destination = area/'import-private'/'snapshot.glb'
        actual = self.store._write
        def replace_source(path, data, decoded=None):
            if path == destination: (candidate/'candidate.glb').write_bytes(b'replaced after validation')
            actual(path, data, decoded)
        with patch.object(self.store, '_write', side_effect=replace_source):
            snapshot = self.store.stage_for_import(JOB, receipt['candidate']['sha256'], receipt['candidate']['provenance_sha256'], destination)
        self.assertEqual(destination.read_bytes(), self.members['candidate.glb'])
        self.assertEqual(snapshot['artifact_sha256'], digest(self.members['candidate.glb']))
        self.refusal('candidate.path', lambda: self.store.stage_for_import(JOB, receipt['candidate']['sha256'], receipt['candidate']['provenance_sha256'], self.project/'escaped.glb'))
        self.refusal('candidate.changed', lambda: self.store.read(JOB))

    def test_import_private_traversal_refused_before_any_destination_publication(self):
        receipt = self.store.publish(JOB, self.members)
        candidate, area = self.store.paths(JOB)
        before = (candidate/'candidate.glb').read_bytes()
        destination = area/'import-private'/'..'/'..'/'..'/'candidates'/JOB/'candidate.glb'
        self.refusal('candidate.path', lambda: self.store.stage_for_import(JOB, receipt['candidate']['sha256'], receipt['candidate']['provenance_sha256'], destination))
        self.assertEqual((candidate/'candidate.glb').read_bytes(), before)
        self.assertFalse((area/'import-private').exists())

    def test_publication_ancestor_swaps_refuse_and_preserve_outside(self):
        import shutil
        from luminumbra_author.meshgen_candidate import RetainedDirectories
        self.store.publish(JOB, self.members)
        candidate, _ = self.store.paths(JOB)
        for ancestor in (self.project, self.store.root, candidate, candidate/'inputs'):
            for hook in ('create', 'replace', 'durability'):
                with self.subTest(ancestor=ancestor.name, hook=hook):
                    outside = self.project.parent/('outside-'+ancestor.name+'-'+hook)
                    backup = ancestor.with_name(ancestor.name+'-original')
                    shutil.copytree(ancestor, outside)
                    probe = outside.with_name(outside.name+'-probe')
                    try: probe.symlink_to(outside, target_is_directory=True)
                    except OSError as error:
                        shutil.rmtree(outside)
                        if os.name == 'nt' and getattr(error, 'winerror', None) == 1314:
                            self.skipTest(str(error))  # ERROR_PRIVILEGE_NOT_HELD
                        raise
                    probe.unlink()
                    before = {p.relative_to(outside): p.read_bytes() for p in outside.rglob('*') if p.is_file()}
                    swapped, blocked = [], []
                    def swap():
                        if swapped or blocked: return
                        try: ancestor.rename(backup)
                        except PermissionError as error:
                            if os.name != 'nt' or getattr(error, 'winerror', None) != 5: raise
                            blocked.append(error); return
                        ancestor.symlink_to(outside, target_is_directory=True); swapped.append(True)
                    verify, replace, fsync = RetainedDirectories.verify, file_io.replace_file, os.fsync
                    checks, published = [0], [False]
                    def check(directories):
                        verify(directories)
                        # All destination ancestors have been retained before creation.
                        if hook == 'create' and candidate/'inputs' in directories.directories:
                            checks[0] += 1
                            if checks[0] == 1: swap()
                    def replacing(*args, **kwargs):
                        if hook == 'replace': swap()
                        result = replace(*args, **kwargs)
                        published[0] = True
                        if hook == 'durability' and os.name == 'nt': swap()
                        return result
                    def flushing(fd):
                        result = fsync(fd)
                        if hook == 'durability' and published[0] and stat.S_ISDIR(os.fstat(fd).st_mode): swap()
                        return result
                    try:
                        with patch.object(RetainedDirectories, 'verify', check), patch.object(file_io, 'replace_file', replacing), patch.object(os, 'fsync', flushing):
                            try: self.store._write(candidate/'inputs'/'finish.log', b'new log')
                            except Refusal as error:
                                self.assertEqual(error.finding['rule_id'], 'candidate.path')
                                self.assertFalse(blocked)
                            else:
                                self.assertTrue(blocked, 'swap completed without refusal')
                        after = {p.relative_to(outside): p.read_bytes() for p in outside.rglob('*') if p.is_file()}
                        self.assertEqual(before, after)
                    finally:
                        if swapped:
                            ancestor.unlink(); backup.rename(ancestor)
                        shutil.rmtree(outside)

    def test_attestation_reorder_duplicate_replay_and_anchor_truncation_refused(self):
        import shutil
        for attack in ('reorder', 'duplicate', 'replay', 'truncate', 'remove_anchor'):
            with self.subTest(attack=attack):
                store = CandidateStore(self.project, attack)
                store.publish(JOB, self.members)
                for i in range(2): store.append_attestation(JOB, 'metrics', f'20261013T12345600000{i}Z', {'score': i})
                candidate, area = store.paths(JOB)
                state = bounded_json((area/'attestations.json').read_bytes())
                if attack == 'reorder':
                    state['entries'].reverse()
                    previous = '0'*64
                    for entry in state['entries']:
                        entry['previous'] = previous; previous = entry['sha256']
                elif attack == 'duplicate':
                    state['entries'].append(deepcopy(state['entries'][-1]))
                    state['entries'][-1]['previous'] = state['entries'][-2]['sha256']
                elif attack == 'replay':
                    store.publish(SECOND, self.members)
                    other_candidate, other_area = store.paths(SECOND)
                    shutil.copytree(candidate/'attestations', other_candidate/'attestations')
                    shutil.copytree(area/'attestation-history', other_area/'attestation-history')
                    (other_area/'attestations.json').write_bytes(canonical(state)+b'\n')
                    self.refusal('attestation.changed', lambda: CandidateStore(self.project, attack).read(SECOND))
                    continue
                elif attack == 'truncate':
                    removed = state['entries'].pop()
                    for suffix in ('', '.sha256'): (candidate/'attestations'/(removed['name']+suffix)).unlink()
                else: (area/'attestations.json').unlink()
                if attack != 'remove_anchor': (area/'attestations.json').write_bytes(canonical(state)+b'\n')
                self.refusal('attestation.changed', lambda: CandidateStore(self.project, attack).read(JOB))

    def test_attestation_publication_internal_crashes_recover_outside_seal_staging(self):
        import subprocess
        import sys
        service = Path(__file__).resolve().parents[1]
        tests = Path(__file__).resolve().parent
        modes = ['creation', 'fsync', 'replacement']
        if os.name == 'nt': modes += ['directory_creation', 'directory_replacement']
        source = r"""
import os, stat, sys
from pathlib import Path
from meshgen_e4_fixture import members
from luminumbra_author import file_io, meshgen_candidate as module
from luminumbra_author.meshgen_candidate import CandidateStore
project, job, mode = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
store = CandidateStore(project, mode)
store.publish(job, members())
candidate, area = store.paths(job)
def pending(): return (area/'attestations.json').exists()
original_open, original_path_open = os.open, Path.open
original_fsync, original_replace = os.fsync, file_io.replace_file
original_directory = module.durable_replace_directory
def opened(path, *args, **kwargs):
    fd = original_open(path, *args, **kwargs)
    if pending() and mode == 'creation' and str(path).startswith('.publish-'): os._exit(23)
    return fd
def path_open(path, *args, **kwargs):
    stream = original_path_open(path, *args, **kwargs)
    if pending() and mode == 'creation' and path.name.startswith('.publish-'): os._exit(23)
    return stream
def fsync(fd):
    if pending() and mode == 'fsync' and stat.S_ISREG(os.fstat(fd).st_mode): os._exit(23)
    return original_fsync(fd)
def replace(src, dst, **kwargs):
    value = original_replace(src, dst, **kwargs)
    if pending() and mode == 'replacement' and Path(dst).name.startswith('metrics.'): os._exit(23)
    return value
def directory(src, dst):
    if pending() and mode == 'directory_creation' and dst == candidate/'attestations': os._exit(23)
    value = original_directory(src, dst)
    if pending() and mode == 'directory_replacement' and dst == candidate/'attestations': os._exit(23)
    return value
os.supports_dir_fd.add(opened)
os.open, Path.open, os.fsync = opened, path_open, fsync
file_io.replace_file, module.durable_replace_directory = replace, directory
store.append_attestation(job, 'metrics', '20261013T123456000001Z', {'score': 1})
"""
        bootstrap = 'import sys; sys.path[:0]='+repr([str(service), str(tests)])+'; exec('+repr(source)+')'
        for mode in modes:
            with self.subTest(mode=mode):
                result = subprocess.run([sys.executable, '-I', '-S', '-B', '-c', bootstrap, str(self.project), JOB, mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 23, result.stderr)
                store = CandidateStore(self.project, mode)
                self.assertEqual(len(store.recover_attestation(JOB)), 1)
                self.assertEqual(len(CandidateStore(self.project, mode).attestations(JOB)), 1)
                candidate, _ = store.paths(JOB)
                self.assertFalse(any(p.name.startswith(('.publish-', '.mkdir-')) for p in candidate.rglob('*')))

    def test_new_directory_ancestor_swap_refused_during_creation(self):
        import shutil
        self.store.publish(JOB, self.members)
        _, area = self.store.paths(JOB)
        outside = self.project/'outside-directory'
        backup = area.with_name(area.name+'-original')
        shutil.copytree(area, outside)
        probe = self.project/'probe'
        try: probe.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            if os.name == 'nt' and getattr(error, 'winerror', None) == 1314:
                self.skipTest(str(error))  # ERROR_PRIVILEGE_NOT_HELD
            raise
        probe.unlink()
        before = {p.relative_to(outside): p.read_bytes() for p in outside.rglob('*') if p.is_file()}
        swapped, blocked = [], []
        mkdir = os.mkdir
        def creating(path, *args, **kwargs):
            result = mkdir(path, *args, **kwargs)
            name = Path(path).name
            if name == 'new-directory' or os.name == 'nt' and name.startswith('.mkdir-'):
                try: area.rename(backup)
                except PermissionError as error:
                    if os.name != 'nt' or getattr(error, 'winerror', None) != 5: raise
                    blocked.append(error); return result
                area.symlink_to(outside, target_is_directory=True); swapped.append(True)
            return result
        try:
            with patch.object(os, 'mkdir', creating):
                try: self.store._write(area/'new-directory'/'snapshot', b'new bytes')
                except Refusal as error:
                    self.assertEqual(error.finding['rule_id'], 'candidate.path')
                    self.assertFalse(blocked)
                else: self.assertTrue(blocked, 'ancestor swap was accepted')
            after = {p.relative_to(outside): p.read_bytes() for p in outside.rglob('*') if p.is_file()}
            self.assertEqual(before, after)
        finally:
            if swapped: area.unlink(); backup.rename(area)

    def test_authoritative_head_refuses_coordinated_suffix_deletion_and_replacement(self):
        import shutil
        for attack in ('suffix', 'replace', 'all'):
            with self.subTest(attack=attack):
                store = CandidateStore(self.project, 'coordinated_'+attack)
                store.publish(JOB, self.members)
                for i in range(2):
                    store.append_attestation(JOB, 'metrics', f'20261013T12345600000{i}Z', {'score': i})
                candidate, area = store.paths(JOB)
                pointer_before = store._head_path(JOB).read_bytes()
                state = bounded_json((area/'attestations.json').read_bytes())
                if attack == 'all':
                    shutil.rmtree(candidate/'attestations')
                    shutil.rmtree(area/'attestation-history')
                    (area/'attestations.json').unlink()
                elif attack == 'suffix':
                    entry = state['entries'].pop()
                    for suffix in ('', '.sha256'): (candidate/'attestations'/(entry['name']+suffix)).unlink()
                    (area/'attestation-history'/'0001.json').unlink()
                    (area/'attestations.json').write_bytes(canonical(state)+b'\n')
                else:
                    entry = state['entries'][-1]
                    entry['payload']['observation'] = {'score': 'replacement'}
                    data = canonical(entry['payload'])+b'\n'; entry['sha256'] = digest(data)
                    (candidate/'attestations'/entry['name']).write_bytes(data)
                    (candidate/'attestations'/(entry['name']+'.sha256')).write_text(entry['sha256']+'\n')
                    (area/'attestation-history'/'0001.json').write_bytes(canonical(entry)+b'\n')
                    (area/'attestations.json').write_bytes(canonical(state)+b'\n')
                fresh = CandidateStore(self.project, store.root.name)
                self.refusal('attestation.changed', lambda: fresh.read(JOB))
                self.refusal('attestation.changed', lambda: fresh.recover_attestation(JOB))
                self.assertEqual(store._head_path(JOB).read_bytes(), pointer_before)

    def test_attestation_pointer_and_candidate_crash_recovery_in_both_orders(self):
        import subprocess
        import sys
        service, tests = Path(__file__).resolve().parents[1], Path(__file__).resolve().parent
        source = r'''
import os, sys
from pathlib import Path
from meshgen_e4_fixture import members
from luminumbra_author.meshgen_candidate import CandidateStore
store = CandidateStore(Path(sys.argv[1]), sys.argv[2]); job = sys.argv[3]
store.publish(job, members())
store.append_attestation(job, 'metrics', '20261013T123456000000Z', {'score': 0})
mode = sys.argv[2]
write, write_head = store._write, store._write_head
def head(job, value):
    write_head(job, value)
    if value['pending'] and mode == 'head_first':
        # The independent prepared payload permits recovery even when a
        # committed head reaches disk before any candidate evidence does.
        value.update(count=value['count']+1, head=value['pending']['sha256'])
        write_head(job, value)
        os._exit(23)
    if mode == 'head_committed' and value['count'] == 2: os._exit(23)
def evidence(path, data, decoded=None):
    write(path, data, decoded)
    if mode == 'candidate_first' and path.name == '0001.json': os._exit(23)
store._write, store._write_head = evidence, head
store.append_attestation(job, 'metrics', '20261013T123456000001Z', {'score': 1})
'''
        bootstrap = 'import sys; sys.path[:0]='+repr([str(service), str(tests)])+'; exec('+repr(source)+')'
        for mode in ('candidate_first', 'head_first', 'head_committed'):
            with self.subTest(mode=mode):
                result = subprocess.run([sys.executable, '-I', '-S', '-B', '-c', bootstrap, str(self.project), mode, JOB],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 23, result.stderr)
                fresh = CandidateStore(self.project, mode)
                recovered = fresh.recover_attestation(JOB)
                self.assertEqual([p['observation']['score'] for p in recovered], [0, 1])
                self.assertEqual(CandidateStore(self.project, mode).attestations(JOB), recovered)
                head = bounded_json(fresh._head_path(JOB).read_bytes())
                self.assertEqual(head['count'], 2); self.assertIsNone(head['pending'])
                self.assertEqual(head['head'], digest(canonical(recovered[-1])+b'\n'))
                candidate, _ = fresh.paths(JOB)
                self.assertFalse(any(p.name.startswith(('.publish-', '.mkdir-')) for p in candidate.rglob('*')))
