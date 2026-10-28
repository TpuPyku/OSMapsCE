"""The offline tests' second region (native_osm/test_offline.bat): tiny.osm 5 degrees north, its town Другоград - a
search from the first region finds it there (the user 2026-10-29: a town of another region on the card).

    python make_twin.py <tiny.osm> <out.osm>
"""
import re
import sys


def twin(text):
    text = re.sub(r'lat="([0-9.]+)"', lambda m: 'lat="%.3f"' % (float(m.group(1)) + 5), text)
    return text.replace('Тестовск', 'Другоград')


if __name__ == '__main__':
    with open(sys.argv[1], encoding='utf-8') as f, open(sys.argv[2], 'w', encoding='utf-8') as o:
        o.write(twin(f.read()))
