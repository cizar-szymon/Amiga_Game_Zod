#!/usr/bin/env python3
"""A text of the release for the Amiga (tools/release/make_release.sh).

  python tools/release/amiga_text.py SRC OUT VERSION

SRC is UTF-8 (docs/release/amiga). OUT is ISO-8859-1, LF: German letters stay,
Polish letters lose their marks (the Amiga has no Polish character set by
default; the same rule as in the OpenDUNE port). @V@ becomes VERSION.
"""
import sys

POLISH = str.maketrans('ąćęłńóśźżĄĆĘŁŃÓŚŹŻ', 'acelnoszzACELNOSZZ')


def main():
    src, out, version = sys.argv[1:4]
    t = open(src, encoding='utf-8').read().replace('@V@', version).translate(POLISH)
    t = t.replace('\r\n', '\n')
    open(out, 'w', encoding='latin-1', newline='\n').write(t)


if __name__ == '__main__':
    main()
