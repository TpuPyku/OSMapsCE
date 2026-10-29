"""The offline tests' second region (native_osm/test_offline.bat): tiny.osm 5 degrees north, its town Другоград - a
search from the first region finds it there (the user 2026-10-29: a town of another region on the card); its side
street named after the first region's town (Тестовская): asked there, "Тестовск" is the street here, the town there;
a village (Дальнее) off its streets.

    python make_twin.py <tiny.osm> <out.osm>
"""
import re
import sys


def twin(text):
    text = re.sub(r'lat="([0-9.]+)"', lambda m: 'lat="%.3f"' % (float(m.group(1)) + 5), text)
    text = text.replace('Тестовск', 'Другоград').replace('Боковая улица', 'Тестовская улица')
    # a village 4 km off, its reach not the streets' (they are Другоград's): a place named in a query, near it
    village = ('  <node id="990001" lat="58.040" lon="50.050"><tag k="place" v="village"/>'
               '<tag k="name" v="Дальнее"/></node>\n')
    return text.replace('</osm>', village + '</osm>')


if __name__ == '__main__':
    with open(sys.argv[1], encoding='utf-8') as f, open(sys.argv[2], 'w', encoding='utf-8') as o:
        o.write(twin(f.read()))
