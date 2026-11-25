#!/usr/bin/env python3
"""The AOCS panel's page, between its HTML source and the C header the
firmware embeds.

The AOCS team edits status.html and generates pagina_html.h from it with a
script of theirs (ferramentas/gera_pagina_h.py) that did not come with the
firmware delivered on 2026-11-25. This one writes the same bytes, so the
header in flight/aocs/arduino/ can be regenerated and checked:

  gera_pagina_h.py HTML HEADER            write HEADER from HTML
  gera_pagina_h.py --extract HEADER HTML  write HTML from HEADER
  gera_pagina_h.py --check HTML HEADER    exit 1 unless HEADER is HTML's

The page goes in a C++ raw string, R"PAGINA(...)PAGINA", so it must not
contain the delimiter.
"""
import argparse
import pathlib
import sys

OPEN = b'static const char PAGINA_HTML[] PROGMEM = R"PAGINA('
CLOSE = b')PAGINA";\n'
RULE = '// ' + '=' * 73 + '\n'


def header(html: bytes) -> bytes:
    if b')PAGINA"' in html:
        raise ValueError('the page contains the raw-string delimiter )PAGINA"')
    banner = (RULE
              + '//  PAGINA_HTML.H - GERADO AUTOMATICAMENTE A PARTIR DA PÁGINA DO PAINEL\n'
              + RULE
              + '// NÃO edite à mão: edite status.html e rode ferramentas/gera_pagina_h.py\n'
              + '// (ou gera_pagina_h.m no MATLAB).\n'
              + f'// Tamanho: {len(html)} bytes\n'
              + RULE
              + '#pragma once\n#include <pgmspace.h>\n\n')
    return banner.encode('utf-8') + OPEN + html + CLOSE


def page(h: bytes) -> bytes:
    start = h.index(OPEN) + len(OPEN)
    end = h.rindex(CLOSE)
    return h[start:end]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--extract', action='store_true', help='HEADER -> HTML')
    g.add_argument('--check', action='store_true', help='HEADER is generated from HTML?')
    ap.add_argument('src', type=pathlib.Path)
    ap.add_argument('dst', type=pathlib.Path)
    a = ap.parse_args()
    if a.extract:
        a.dst.write_bytes(page(a.src.read_bytes()))
        return 0
    want = header(a.src.read_bytes())
    if a.check:
        if a.dst.read_bytes() != want:
            print(f'[ERRO] {a.dst} não é o gerado de {a.src}: rode tools/aocs/gera_pagina_h.py',
                  file=sys.stderr)
            return 1
        print(f'[OK] {a.dst} = {a.src} ({len(want)} bytes)')
        return 0
    a.dst.write_bytes(want)
    return 0


if __name__ == '__main__':
    sys.exit(main())
