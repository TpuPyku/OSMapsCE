"""A region's zip as the release has it, for native_osm/test_offline (offmaps.cpp unpacks it): route.grf deflated,
search.idx stored, region.txt - and the region.txt of the test region tiny (on the card: its date).

    python make_test_zip.py <out>/region.zip <tiny's folder>
"""
import os
import sys
import zipfile

if __name__ == '__main__':
    out, tiny = sys.argv[1], sys.argv[2]
    grf = bytes(range(256)) * 300                       # compresses: deflated
    with zipfile.ZipFile(out, 'w') as z:
        z.writestr(zipfile.ZipInfo('route.grf'), grf, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr(zipfile.ZipInfo('search.idx'), b'IDX' * 10, compress_type=zipfile.ZIP_STORED)
        z.writestr(zipfile.ZipInfo('region.txt'), b'name=zipped\ndate=2026-10-29\n', compress_type=zipfile.ZIP_DEFLATED)
    with open(os.path.join(tiny, 'region.txt'), 'wb') as f:
        f.write(b'name=tiny\ndate=2026-10-20\n')
