#!/usr/bin/env python3
"""Every Russian string the program shows has a line in OSMapsCE\\Lang\\en.txt, with the same formats, and every format
built from one goes through Tr(). Exit code: the problems found."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANG = os.path.join(HERE, '..', 'OSMapsCE', 'Lang', 'en.txt')
SOURCES = ('main.cpp', 'net.cpp', 'offmaps.cpp', 'route.cpp', 'offline.cpp', 'util.cpp', 'vtile.cpp', 'cache.cpp', 'lang.cpp')
# not interface text: the keyboard's own key, and what the language itself is called
KEEP = {'РУС', 'Язык / Language', 'Русский'}
LITERAL = re.compile(r'L"((?:[^"\\]|\\.)*)"')
CYRILLIC = re.compile('[А-Яа-яЁё]')
SPEC = re.compile(r'%(?!%)[-+ #0]*[0-9.]*[lh]*[a-zA-Z]')


def code_of(path):
    """The source without its comments (strings kept)."""
    text = open(path, encoding='utf-8-sig').read()
    return re.sub(r'//[^\n]*|/\*.*?\*/|("(?:[^"\\\n]|\\.)*")', lambda m: m.group(1) or '', text, flags=re.S)


def statements(code, head):
    """The calls of head(...), whole: their text."""
    for m in re.finditer(re.escape(head) + r'\(', code):
        depth, i, in_str = 1, m.end(), False
        while depth and i < len(code):
            c = code[i]
            if in_str:
                i += c == '\\'
                in_str = c != '"'
            elif c == '"':
                in_str = True
            else:
                depth += (c == '(') - (c == ')')
            i += 1
        yield code[m.start():i]


def main():
    pairs, bad = {}, []
    for n, line in enumerate(open(LANG, encoding='utf-8-sig'), 1):
        line = line.rstrip('\r\n')
        if not line or line.startswith('#'):
            continue
        if '\t' not in line:
            bad.append('en.txt:%d: no tab' % n)
            continue
        ru, en = line.split('\t', 1)
        if ru in pairs:
            bad.append('en.txt:%d: twice: %s' % (n, ru))
        pairs[ru] = en
        if SPEC.findall(ru.replace('%%', '')) != SPEC.findall(en.replace('%%', '')):
            bad.append('en.txt:%d: formats differ: %s' % (n, ru))
        if CYRILLIC.search(en):
            bad.append('en.txt:%d: Russian in the translation: %s' % (n, en))
    used = set()
    for name in SOURCES:
        code = code_of(os.path.join(HERE, name))
        for s in LITERAL.findall(code):
            if CYRILLIC.search(s) and s not in KEEP:
                used.add(s)
                if s not in pairs:
                    bad.append('%s: no translation: %s' % (name, s))
        for call in statements(code, '_snwprintf'):
            for m in LITERAL.finditer(call):
                if CYRILLIC.search(m.group(1)) and not call[:m.start()].endswith('Tr('):
                    bad.append('%s: a format without Tr(): %s' % (name, m.group(1)))
    for ru in sorted(set(pairs) - used):
        bad.append('en.txt: no longer in the sources: %s' % ru)
    for b in bad:
        print(b.encode('ascii', 'backslashreplace').decode())
    print('lang_check: %d strings, %d lines in en.txt, %d problems' % (len(used), len(pairs), len(bad)))
    return len(bad)


if __name__ == '__main__':
    sys.exit(main())
