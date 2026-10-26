#!/usr/bin/env python3
"""Maintain the report-only daily Windows tracking issue using gh and GH_TOKEN."""
from __future__ import annotations

import json
import os
import subprocess

TITLE = "Daily Windows run"
LABEL = "windows-daily"


def gh(*arguments: str) -> str:
    result = subprocess.run(["gh", *arguments], check=True, capture_output=True,
                            text=True, encoding="utf-8", timeout=120)
    return result.stdout


def pages(endpoint: str) -> list[dict]:
    # gh follows every Link page; the process deadline bounds the entire scan.
    response = json.loads(gh("api", "--paginate", "--slurp", endpoint))
    if not isinstance(response, list):
        raise ValueError("Invalid paginated GitHub response")
    return response


def report(environment: dict[str, str]) -> None:
    repository = environment["GITHUB_REPOSITORY"]
    run_id = environment["GITHUB_RUN_ID"]
    attempt = environment["GITHUB_RUN_ATTEMPT"]
    sha = environment.get("WINDOWS_HEAD_SHA") or environment["GITHUB_SHA"]
    url = f"{environment['GITHUB_SERVER_URL']}/{repository}/actions/runs/{run_id}/attempts/{attempt}"
    failed = sorted(job["name"] for page in pages(
        f"repos/{repository}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100")
        for job in page["jobs"] if job["status"] == "completed" and job["conclusion"] != "success")
    passed = environment["BUILD_RESULT"] == "success" and not failed
    issues = [issue for page in pages(f"repos/{repository}/issues?state=open&per_page=100")
              for issue in page if issue["title"] == TITLE and "pull_request" not in issue]
    issue = min(issues, key=lambda row: row["number"]) if issues else None
    if passed and issue is None:
        return
    body = f"Run: {url}\nHead SHA: `{sha}`\n"
    if passed:
        gh("issue", "close", str(issue["number"]), "--repo", repository, "--comment",
           body + "Daily Windows validation passed; closing this tracking issue.")
        return
    body += "Failed jobs:\n" + "\n".join(f"- {name}" for name in failed)
    if not failed:
        body += f"\n- Windows build ({environment['BUILD_RESULT']})"
    if issue is None or not any(label["name"] == LABEL for label in issue["labels"]):
        labels = [label for page in pages(f"repos/{repository}/labels?per_page=100") for label in page]
        if not any(label["name"] == LABEL for label in labels):
            gh("label", "create", LABEL, "--repo", repository, "--description", "Daily Windows validation failures")
    if issue is None:
        issue_url = gh("issue", "create", "--repo", repository, "--title", TITLE,
                       "--label", LABEL, "--body", "Daily Windows proof is report-only for landings and gates the v0.3.1 tag.\n\n" + body).strip()
        gh("issue", "comment", issue_url, "--repo", repository, "--body", body)
    else:
        if not any(label["name"] == LABEL for label in issue["labels"]):
            gh("issue", "edit", str(issue["number"]), "--repo", repository, "--add-label", LABEL)
        gh("issue", "comment", str(issue["number"]), "--repo", repository, "--body", body)


if __name__ == "__main__":
    report(dict(os.environ))
