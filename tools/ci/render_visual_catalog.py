#!/usr/bin/env python3
"""Render the complete visual catalog to Markdown without changing evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from validate_visual_catalog import CATALOG, FAMILIES, ROOT, read_json, summary, validate


def files(catalog: dict) -> dict[str, str]:
    result = {}
    rows = catalog["rows"]
    index = ["# Visual scenario roster", "", "Generated from [visual-catalog.json](visual-catalog.json). "
             "See the [packet and approval policy](visual-catalog.md).", "", summary(catalog) + ".", "",
             "A catalog consistency pass establishes bookkeeping only. Every original and added group "
             "remains part of completion; source fixtures and diagnostic checks are not native acceptance.", ""]
    for prefix, (_, family) in FAMILIES.items():
        slug = family.replace("/", "-")
        group = [row for row in rows if row["family"] == family]
        index += [f"## {family}", "", "| ID | Scenario | Capture producer | Execution / approval |",
                  "|---|---|---|---|"]
        page = [f"# {family} visual scenarios", "", "Generated from [the catalog](visual-catalog.json). "
                "[Roster](visual-catalog-roster.md) · [shared packet contract](visual-catalog.md).", ""]
        for row in group:
            scenario_id = row["id"]
            index.append(f"| [{scenario_id}](visual-catalog-{slug}.md) | {row['name']} | "
                         f"{row['capture']['state']} | {row['execution_state']} / {row['approval']['state']} |")
            page += [f"## {scenario_id}", "", f"**{row['name']}**", "", row["visual_goal"], "",
                     f"Origin: {row['origin']}. " + ("Parents: " + ", ".join(row["parents"]) + ". "
                     if row["parents"] else "") + f"Priority: {row['priority']}. "
                     f"Execution: **{row['execution_state']}**. Approval: **{row['approval']['state']}**.", "",
                     "Fixture: " + row["fixture"]["description"], "", "Source links:", ""]
            if row["fixture"]["source_paths"]:
                page += [f"- [{p}](https://github.com/d-addison/luminumbra/blob/{row['reconciliation']['source_commit']}/{p})" for p in row["fixture"]["source_paths"]]
            else:
                page += ["- Missing: this fixture still needs implementation and a reproducible producer."]
            page += ["", "Required variants:", ""]
            page += ["- " + variant for variant in row["required_variants"]]
            page += ["", "Automated temporal evidence: **" +
                     ("required" if row["temporal_evidence_required"] else "no separate motion claim") + "**.", "",
                     "Implementation dependencies:", ""]
            page += [f"- {dep}: {catalog['dependencies'][dep]}" for dep in row["dependencies"]]
            page += ["", "Capture: **" + row["capture"]["state"] + "**. " + row["capture"]["scope"], ""]
            if row["capture"]["command"] is not None:
                page += ["Arguments are a command template, not a newly executed run. Replace placeholders "
                         "with owned native paths and a fresh output directory; reserve the native slot first.", "",
                         "```text", " ".join(row["capture"]["command"]), "```", ""]
                if row["capture"]["environment"]:
                    page += ["Required environment: `" + json.dumps(row["capture"]["environment"], sort_keys=True) + "`.", ""]
            page += ["Acceptance checks:", ""]
            page += ["- " + check for check in row["acceptance_checks"]]
            page += ["", f"Source reconciliation at `{row['reconciliation']['source_commit']}`: "
                     + row["reconciliation"]["finding"], "", "Retained evidence:", ""]
            if row["evidence"]:
                for evidence in row["evidence"]:
                    page += [f"- {evidence['kind']} at executed source `{evidence['source_commit']}`; "
                             f"[receipt]({evidence['receipt'].removeprefix('docs/')})."]
                    page += [f"- [{Path(a['path']).name}]({a['path'].removeprefix('docs/')}) — SHA-256 `{a['sha256']}`."
                             for a in evidence["artifacts"]]
            else:
                page += ["- None qualified for this catalog row."]
            page += ["", "Next action: " + row["next_action"], ""]
        index.append("")
        result[f"docs/visual-catalog-{slug}.md"] = "\n".join(page)
    result["docs/visual-catalog-roster.md"] = "\n".join(index)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail when committed pages differ; write nothing")
    args = parser.parse_args()
    catalog = read_json(ROOT / CATALOG)
    errors = validate(catalog, ROOT)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    for relative, content in files(catalog).items():
        path = ROOT / relative
        if args.check:
            if not path.is_file() or path.read_text(encoding="utf-8") != content:
                errors.append("generated catalog page differs: " + relative)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Visual catalog pages: " + ("current" if args.check else "written"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
