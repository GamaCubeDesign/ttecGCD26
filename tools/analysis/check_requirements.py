#!/usr/bin/env python3
"""
Checks that every quotation in docs/requisitos.md is literal text from the
competition rules PDF.

docs/requisitos.md exists so the team can work without opening the PDF. That
only holds if its quotations are exact: a paraphrase that drifts from the
rules is worse than no transcription at all, because it is trusted. This
script extracts the PDF text with pdftotext (poppler-utils) and confirms that
each blockquote in the markdown occurs in it.

    python3 tools/analysis/check_requirements.py

When the organisers publish a new version of the rules, replace the PDF and
run this again: every quotation that no longer matches is reported.

Comparison ignores whitespace, bullet glyphs, markdown syntax and the
difference between typographic and straight quotes and dashes. Nothing else
is normalised — a changed word, number or unit is a mismatch.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_PDF = ROOT / "docs" / "cubedesign2026.pdf"
DEFAULT_MD = ROOT / "docs" / "requisitos.md"

TYPOGRAPHY = str.maketrans({
    "“": '"', "”": '"', "‘": "'", "’": "'",
    "–": "-", "—": "-",
    "●": " ", "○": " ",  # the PDF's bullet glyphs
    "​": "",                  # zero-width space after every bullet and ID
    "\f": " ",                     # page breaks
})


def normalise(text):
    return re.sub(r"\s+", " ", text.translate(TYPOGRAPHY)).strip()


def pdf_text(pdf):
    if shutil.which("pdftotext") is None:
        sys.exit("pdftotext not found; install poppler-utils")
    out = subprocess.run(["pdftotext", str(pdf), "-"],
                         check=True, capture_output=True, text=True)
    return normalise(out.stdout)


def quotations(markdown):
    """Yields (line number, text) for each blockquote, markdown stripped."""
    block, start = [], 0
    for number, line in enumerate(markdown.splitlines() + [""], start=1):
        if line.startswith(">"):
            if not block:
                start = number
            body = line[1:].strip()
            body = re.sub(r"^- ", "", body)      # unordered list marker
            body = body.replace("**", "")        # bold
            body = body.rstrip("\\")             # hard line break
            block.append(body)
        elif block:
            yield start, normalise(" ".join(block))
            block = []


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pdf", type=pathlib.Path, default=DEFAULT_PDF)
    parser.add_argument("--md", type=pathlib.Path, default=DEFAULT_MD)
    args = parser.parse_args()

    source = pdf_text(args.pdf)
    missing, total = [], 0
    for line, quote in quotations(args.md.read_text(encoding="utf-8")):
        total += 1
        if quote not in source:
            missing.append((line, quote))

    for line, quote in missing:
        print(f"{args.md.name}:{line}: not found in the PDF:")
        print(f"    {quote[:110]}{'...' if len(quote) > 110 else ''}")

    print(f"{total - len(missing)} of {total} quotations match "
          f"{args.pdf.name} verbatim")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
