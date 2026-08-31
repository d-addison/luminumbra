#!/usr/bin/env python3
"""Validate that generated Doxygen HTML has no broken maintained local links."""

from __future__ import annotations

import argparse
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


class LinkParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attribute = "href" if tag in {"a", "link"} else "src" if tag in {"img", "script"} else None
        if attribute is None:
            return
        for key, value in attrs:
            if key == attribute and value:
                self.links.append(value)


def broken_links(html_root: Path) -> list[str]:
    root = html_root.resolve()
    html_files = sorted(root.rglob("*.html"))
    if not html_files or not (root / "index.html").is_file():
        return ["generated documentation is missing index.html"]

    failures: list[str] = []
    for page in html_files:
        parser = LinkParser()
        parser.feed(page.read_text(encoding="utf-8", errors="replace"))
        for raw_link in parser.links:
            parsed = urlsplit(raw_link)
            if parsed.scheme or parsed.netloc or raw_link.startswith(("#", "mailto:", "javascript:", "data:")):
                continue
            relative = unquote(parsed.path)
            if not relative:
                continue
            target = root / relative.lstrip("/") if relative.startswith("/") else page.parent / relative
            try:
                resolved = target.resolve()
                resolved.relative_to(root)
            except ValueError:
                failures.append(f"{page.relative_to(root)}: link escapes documentation root: {raw_link}")
                continue
            if not resolved.exists():
                failures.append(f"{page.relative_to(root)}: missing local target: {raw_link}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--html", type=Path, required=True)
    args = parser.parse_args()
    failures = broken_links(args.html)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("documentation links: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
