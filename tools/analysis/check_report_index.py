#!/usr/bin/env python3
"""docs/relatorios/index.html lists every report, and only reports that exist.

The reports keep their dated names (they are cited by path from the ADRs,
the READMEs and the bench notes); the index is what organises them. This
check, in CTest as relatorios_index, keeps a new report from being left out
of it, and a link in it from pointing at nothing.

  check_report_index.py [REPO]
"""
import pathlib
import re
import sys


def main():
    repo = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    rel = repo / "docs/relatorios"
    index = (rel / "index.html").read_text(encoding="utf-8")
    links = set(re.findall(r'href="([^"#:]+)"', index))
    reports = {p.name for p in rel.glob("20*.html")}
    missing = sorted(reports - links)
    broken = sorted(l for l in links if not (rel / l).exists())
    for name in missing:
        print(f"check_report_index: {name} is not in docs/relatorios/index.html")
    for link in broken:
        print(f"check_report_index: index.html links to {link}, which does not exist")
    if missing or broken:
        return 1
    print(f"check_report_index: {len(reports)} reports, all in the index")
    return 0


if __name__ == "__main__":
    sys.exit(main())
