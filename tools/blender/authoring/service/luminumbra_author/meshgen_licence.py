"""Pure component-chain admission gate. Evidence bytes are supplied by the parent.

This function never reads files, invents acknowledgements, or accepts a backend's
eligibility assertion. The caller reads committed notes without following links.
"""
from .contracts import canonical, digest
from .meshgen_registry import (ACK, ATTESTATION, COUNSEL, WAIVER, validate_schema)

PRECEDENCE = ("blocked", "internal_only", "counsel", "internal_ok", "shippable")
CLAUSES = ("non_commercial", "territory_restricted", "mau_threshold", "revenue_cap",
           "no_improve_ai", "outputs_territory_bound", "copyleft_dependency",
           "attribution_required", "use_restrictions", "gated_access", "unknown")
PREREQUISITES = {"revenue_cap": "revenue_under_cap", "no_improve_ai": "no_ai_improvement",
                 "gated_access": "registration_done"}


def gate(card, policy, notes=None):
    """Return deterministic status, all reasons, and independent run refusals.

    notes maps committed note IDs to their exact bytes (not to a boolean saying
    they were checked). A counsel note uses the same notes/<id>.md convention.
    Stability revenue clauses additionally require registration.
    """
    notes = notes or {}
    reasons, findings, components, acknowledgements = [], [], [], []

    def reason(status, code, component):
        reasons.append({"status": status, "reason": code, "component": component})

    def finding(code, message, field=""):
        from .contracts import Refusal
        item = Refusal(code, message, field).finding
        if item not in findings:
            findings.append(item)

    def verified(record, id_key, hash_key):
        data = notes.get(record.get(id_key))
        return isinstance(data, bytes) and digest(data) == record.get(hash_key)

    attestations = {}
    for name in PREREQUISITES.values():
        record = policy.get("attestations", {}).get(name, {"value": False})
        validate_schema(record, ATTESTATION)
        attestations[name] = record["value"] is True and verified(record, "note_id", "note_sha256")
        if record["value"] and not attestations[name]:
            finding("config.attestation_unverified", "Attestation note bytes do not match the committed hash.", name)

    inventory = card.get("components")
    if not inventory:
        reason("counsel", "inventory_missing", "undeclared")
    records = list(inventory or [])
    chain = card.get("dataset_chain", {})
    records.extend({"name": "dataset:" + str(i), "licence": item, "dataset_lineage": True}
                   for i, item in enumerate(chain.get("licences", [])))
    if "non_commercial" in chain.get("clauses", []):
        reason("blocked", "non_commercial", "dataset_chain")
    if "unknown" in chain.get("clauses", []):
        reason("counsel", "unknown", "dataset_chain")
    seen = {}
    counsel_ids = set()
    ack_required = set()
    for index, component in enumerate(records):
        licence = component.get("licence") or {}
        if not component.get("dataset_lineage") and licence.get("executed", True) is False:
            continue
        name = component.get("name", "component:" + str(index))
        identity = licence.get("id", "undeclared")
        text_hash = licence.get("text_sha256", "0" * 64)
        clauses = licence.get("clauses", [])
        if not licence.get("id") or not licence.get("text_sha256"):
            reason("counsel", "licence_missing", name)
            counsel_ids.add(identity)
            ack_required.add((identity, text_hash))
        if identity in seen and seen[identity] != text_hash:
            reason("counsel", "licence_conflict", name)
            counsel_ids.add(identity)
            ack_required.add((identity, text_hash))
            ack_required.add((identity, seen[identity]))
        seen[identity] = text_hash
        components.append({"id": identity, "text_sha256": text_hash,
                           "clauses": sorted(clauses), "executed": True})
        if set(clauses) - {"attribution_required", "use_restrictions"}:
            ack_required.add((identity, text_hash))
        for clause in sorted(clauses):
            if clause == "non_commercial":
                reason("blocked", clause, name)
            elif clause in ("territory_restricted", "mau_threshold", "outputs_territory_bound"):
                reason("internal_only", clause, name)
            elif clause == "unknown" or clause not in CLAUSES:
                reason("counsel", clause, name)
                counsel_ids.add(identity)
            elif clause == "copyleft_dependency":
                reason("internal_ok", clause, name)
            elif clause in PREREQUISITES:
                prerequisites = [PREREQUISITES[clause]]
                if clause == "revenue_cap":
                    prerequisites.append("registration_done")
                for prerequisite in prerequisites:
                    ok = attestations[prerequisite]
                    reason("internal_ok" if ok else "internal_only", prerequisite, name)
                    if not ok:
                        finding("licence.prerequisite_unverified", "A mandatory licence prerequisite is unverified.", prerequisite)

    waivers = policy.get("dataset_chain_waivers", [])
    for waiver in waivers:
        validate_schema(waiver, WAIVER)
    waived = any(item["backend_id"] == card.get("backend", {}).get("id") for item in waivers)
    audited = chain.get("audited") is True
    if not audited and not waived:
        reason("internal_ok", "dataset_chain_unaudited", "dataset_chain")
    status = min((item["status"] for item in reasons), key=PRECEDENCE.index, default="shippable")
    if status == "blocked" or status not in policy.get("allowed_status", ["shippable"]):
        finding("licence.status_not_allowed", "The computed licence status is not admitted by project policy.")
    acks = policy.get("internal_acknowledgements", [])
    for ack in acks:
        validate_schema(ack, ACK)
    if status != "shippable":
        for component in components:
            if (component["id"], component["text_sha256"]) not in ack_required:
                continue
            matches = [ack for ack in acks if ack["licence_id"] == component["id"]
                       and ack["text_sha256"] == component["text_sha256"]
                       and ack["scope"] == "internal_only"]
            if matches:
                acknowledgements.append(component["id"])
            else:
                finding("licence.ack_missing", "Internal admission requires an acknowledgement bound to licence ID and text hash.", component["id"])
    if status == "counsel" or counsel_ids or not inventory:
        counsel = policy.get("counsel_notes", [])
        for note in counsel:
            validate_schema(note, COUNSEL)
        required = counsel_ids or set(seen) or {"undeclared"}
        covered = set()
        for note in counsel:
            if note["outcome"] == "cleared_internal" and verified(note, "counsel_note_id", "text_sha256"):
                covered.update(note["covers_licence_ids"])
        if not required <= covered:
            finding("licence.counsel_missing", "Counsel admission requires hash-verified notes covering every unresolved licence.")
    result = {"status": status, "shippable": status == "shippable", "components": components,
              "dataset_chain_audited": audited, "ack_ids": sorted(set(acknowledgements)),
              "reasons": reasons, "findings": findings, "runnable": not findings}
    result["sha256"] = digest(canonical(result))
    return result


def candidate_eligibility(licence, backend_kind, backend_id, *, waived=()):
    """Trusted parent conjunction; mock permission never grants shipping rights."""
    return {"shippable": licence["shippable"] and backend_kind != "mock" and not waived,
            "origin": "backend:mock" if backend_kind == "mock" else "backend:" + backend_id}
