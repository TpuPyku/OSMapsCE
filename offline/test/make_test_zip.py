"""A region's zip as the release has it, for native_osm/test_offline (offmaps.cpp unpacks it): route.grf deflated,
search.idx stored, region.txt - and the region.txt of the test region tiny (on the card: its date); beside it
full.zip and overview.zip, as on Yandex Disk.

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
    # the region whole and the overview as on Yandex Disk (release_regions.stored_zip): all stored - tiles.vtp over
    # a few of the unit's 64 KB pieces
    folder = os.path.dirname(out)
    with zipfile.ZipFile(os.path.join(folder, 'full.zip'), 'w', zipfile.ZIP_STORED) as z:
        z.writestr('route.grf', grf)
        z.writestr('search.idx', b'IDX' * 10)
        tiles = zipfile.ZipInfo('tiles.vtp')
        tiles.extra = b'\xfe\xca\x04\x00abcd'   # (an extra field: the data after it)
        z.writestr(tiles, bytes(range(251)) * 1000)
        z.writestr('region.txt', b'name=zz\ndate=2026-10-30\ntiles=20261030\n')
    with zipfile.ZipFile(os.path.join(folder, 'overview.zip'), 'w', zipfile.ZIP_STORED) as z:
        z.writestr('overview.vtp', b'OVERVIEW' * 100)
        z.writestr('region.txt', b'name=overview\ntiles=20261030\n')
