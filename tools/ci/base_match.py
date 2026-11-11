#!/usr/bin/env python3
"""Check a pull request's base against injected ci.yml workflow-run JSON."""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import sys
from urllib.error import URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


CI_EVENTS = frozenset(("opened", "synchronize", "reopened", "ready_for_review",
                       "converted_to_draft", "labeled", "unlabeled"))
RECOVERY = ("Re-trigger ci.yml with synchronize (push a new head), reopened, "
            "ready_for_review, converted_to_draft, labeled or unlabeled "
            "(opened also starts CI). Re-running an old run retains its old base.")
# The filtered Actions endpoint exposes at most 1,000 runs, 100 per page.
MAX_RUNS = 1000
MAX_PAGE_BYTES = 8 * 1024 * 1024


def evaluate(event, api):
    """Return a printable verdict; invalid or incomplete evidence raises ValueError."""
    pr = event["pull_request"]
    number, head, base = pr["number"], pr["head"]["sha"], pr["base"]["ref"]
    if type(number) is not int or number <= 0 or not isinstance(head, str) or not head or not isinstance(base, str) or not base:
        raise ValueError("missing pull-request number, head SHA or current base")
    current = f"base={base!r} head={head!r}"
    if event["action"] in CI_EVENTS:
        return f"BASEMATCH {current}: {event['action']} starts CI against this base."
    if event["action"] != "edited":
        raise ValueError("unsupported pull-request action")
    runs = api["workflow_runs"]
    if not isinstance(runs, list) or type(api["total_count"]) is not int or not 0 <= len(runs) == api["total_count"] <= MAX_RUNS:
        raise ValueError("incomplete or oversized workflow-run listing")
    ordered = []
    for run in runs:
        if run["event"] != "pull_request" or run["head_sha"] != head or type(run["id"]) is not int or run["id"] <= 0:
            raise ValueError("workflow-run identity does not match the requested head/event")
        created = datetime.strptime(run["created_at"], "%Y-%m-%dT%H:%M:%SZ")
        ordered.append((created, run["id"], run))
    if len({item[1] for item in ordered}) != len(ordered):
        raise ValueError("duplicate workflow-run ids in listing")
    for _, run_id, run in sorted(ordered, key=lambda item: item[:2], reverse=True):
        pulls = run.get("pull_requests")
        if not isinstance(pulls, list) or not pulls or any(not isinstance(pull, dict) or type(pull.get("number")) is not int for pull in pulls):
            raise ValueError(f"ci.yml run {run_id} has no usable pull-request/base metadata (including fork runs)")
        matches = [pull for pull in pulls if pull["number"] == number]
        if not matches:
            continue
        if len(matches) != 1:
            raise ValueError(f"ci.yml run {run_id} has ambiguous pull-request metadata")
        recorded_base = matches[0].get("base")
        recorded = recorded_base.get("ref") if isinstance(recorded_base, dict) else None
        if not isinstance(recorded, str) or not recorded:
            raise ValueError(f"ci.yml run {run_id} has no recorded base")
        if recorded != base:
            raise ValueError(f"BASEMISMATCH ci.yml run {run_id} base={recorded!r}; current {current}")
        return f"BASEMATCH ci.yml run {run_id} {current}"
    raise ValueError(f"no ci.yml pull_request run records a base for this pull request; current {current}")


def fetch_runs(repository, head, token, opener=urlopen):
    """Read every filtered API page, refusing truncated or inconsistent listings.

    The workflow-runs REST schema records pull_requests[].base.ref:
    https://docs.github.com/en/rest/actions/workflow-runs#list-workflow-runs-for-a-workflow
    This is run evidence; a live pull-request lookup would lose the previous base.
    """
    if (not re.fullmatch(r"[A-Za-z0-9-]+/[A-Za-z0-9_.-]+", repository)
            or repository.split("/")[-1] in (".", "..")
            or not re.fullmatch(r"[0-9a-fA-F]{40}", head)):
        raise ValueError("invalid repository or head SHA for API lookup")
    if not token:
        raise ValueError("missing workflow token")
    runs, total = [], None
    for page in range(1, MAX_RUNS // 100 + 1):
        query = urlencode({"event": "pull_request", "head_sha": head, "per_page": 100, "page": page})
        request = Request(f"https://api.github.com/repos/{repository}/actions/workflows/ci.yml/runs?{query}",
                          headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
                                   "X-GitHub-Api-Version": "2022-11-28"})
        with opener(request, timeout=20) as response:
            payload = response.read(MAX_PAGE_BYTES + 1)
        if len(payload) > MAX_PAGE_BYTES:
            raise ValueError("workflow-run API page exceeds byte limit")
        data = json.loads(payload)
        count, batch = data["total_count"], data["workflow_runs"]
        if type(count) is not int or not 0 <= count <= MAX_RUNS or not isinstance(batch, list) or len(batch) > 100:
            raise ValueError("invalid or truncated workflow-run API page")
        if total is not None and total != count:
            raise ValueError("workflow-run listing changed during pagination; edit again to retry")
        total = count
        runs.extend(batch)
        if len(runs) == total:
            return {"total_count": total, "workflow_runs": runs}
        if not batch or len(runs) > total:
            raise ValueError("incomplete workflow-run API pagination")
    raise ValueError("workflow-run pagination limit reached")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", type=Path, default=os.environ.get("GITHUB_EVENT_PATH"))
    parser.add_argument("--api-json", type=Path, help="inject a complete workflow-run listing for offline checks")
    args = parser.parse_args(argv)
    try:
        if args.event is None:
            raise ValueError("missing event JSON")
        event = json.loads(args.event.read_text(encoding="utf-8"))
        print(f"Checking base={event['pull_request']['base']['ref']!r} "
              f"head={event['pull_request']['head']['sha']!r}")
        api = None
        if event["action"] == "edited":
            api = (json.loads(args.api_json.read_text(encoding="utf-8")) if args.api_json else
                   fetch_runs(os.environ.get("GITHUB_REPOSITORY", ""), event["pull_request"]["head"]["sha"],
                              os.environ.get("GH_TOKEN", "")))
        print(evaluate(event, api))
        return 0
    except (OSError, URLError, ValueError, KeyError, TypeError) as error:
        print(f"Base match check failed closed: {error}. {RECOVERY}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
