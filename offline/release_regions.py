"""The regions' offline data for GitHub Releases (the user 2026-10-27: the graph and the search there, built by
Actions - .github/workflows/maps.yml; the tiles are not ours: the PC app cuts them out of Protomaps' build).

    python release_regions.py list                 the regions' names, JSON (the workflow's matrix)
    python release_regions.py build <name> <dist>  <dist>/<name>.zip (route.grf, search.idx, region.txt) and
                                                   <dist>/<name>.json (its catalog entry)
    python release_regions.py catalog <dist> [previous.json]
                                                   <dist>/catalog.json of the entries there, the previous catalog's
                                                   regions not built this time kept (their zips stay in the release)
    python release_regions.py disk <dist>          the overview whole up, the disk's folder public: into the catalog

With YANDEX_DISK_TOKEN set (the user 2026-10-30: GitHub's downloads slow) "build" also cuts the region's tiles out of
Protomaps' build and puts the region whole (graph, search, tiles) on the owner's Yandex Disk, DISK/<name>.zip - the
big files stored, not deflated (OSMapsCE unpacks them piece by piece: no memory for them whole); its entry's "full".

A catalog entry: name, date (the OSM extract's), zip, size, bbox (minLon, minLat, maxLon, maxLat: the road graph's
bounds, for the tiles' cut). The release: the tag "maps" (a pre-release: never the program's latest), its assets
replaced on each run - the app reads ASSETS + "catalog.json".
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import time
import zipfile

import make_graph
import make_region
import pmextract
import yadisk

ASSETS = 'https://github.com/TpuPyku/OSMapsCE/releases/download/maps/'
FILES = ('route.grf', 'search.idx', 'region.txt')
DISK = 'app:/maps'   # the regions whole on the owner's Yandex Disk - the app's folder (Приложения\<app>\maps: the
                     # token's only access, the user 2026-10-30), public
TILES_Z = 15                   # the tiles' deepest zoom (Protomaps': the house numbers)


# The regions' Russian names, for the app's list (the catalog's title; a region not here: its name)
TITLES = {
    'adygea_republic': 'Адыгея', 'altai_krai': 'Алтайский край', 'altai_republic': 'Республика Алтай',
    'amur_oblast': 'Амурская область', 'arkhangelsk_oblast': 'Архангельская область',
    'astrakhan_oblast': 'Астраханская область', 'bashkortostan_republic': 'Башкортостан',
    'belgorod_oblast': 'Белгородская область', 'bryansk_oblast': 'Брянская область', 'buryatia_republic': 'Бурятия',
    'chechen_republic': 'Чечня', 'chelyabinsk_oblast': 'Челябинская область',
    'chukotka_autonomous_okrug': 'Чукотский автономный округ', 'chuvash_republic': 'Чувашия',
    'crimea': 'Крым и Севастополь', 'dagestan_republic': 'Дагестан', 'donetsk': 'Донецк',
    'ingushetia_republic': 'Ингушетия', 'irkutsk_oblast': 'Иркутская область', 'ivanovo_oblast': 'Ивановская область',
    'jewish_autonomous_oblast': 'Еврейская автономная область', 'kabardino_balkar_republic': 'Кабардино-Балкария',
    'kaliningrad_oblast': 'Калининградская область', 'kalmykia_republic': 'Калмыкия',
    'kaluga_oblast': 'Калужская область', 'kamchatka_krai': 'Камчатский край',
    'karachay_cherkess_republic': 'Карачаево-Черкесия', 'karelia_republic': 'Карелия',
    'kemerovo_oblast': 'Кемеровская область', 'khabarovsk_krai': 'Хабаровский край', 'khakassia_republic': 'Хакасия',
    'khanty_mansi_autonomous_okrug': 'Ханты-Мансийский автономный округ', 'kirov_oblast': 'Кировская область',
    'komi_republic': 'Коми', 'kostroma_oblast': 'Костромская область', 'krasnodar_krai': 'Краснодарский край',
    'krasnoyarsk_krai': 'Красноярский край', 'kurgan_oblast': 'Курганская область', 'kursk_oblast': 'Курская область',
    'leningrad_oblast': 'Санкт-Петербург и Ленинградская область', 'lipetsk_oblast': 'Липецкая область',
    'luhansk': 'Луганск', 'magadan_oblast': 'Магаданская область', 'mari_el_republic': 'Марий Эл',
    'mordovia_republic': 'Мордовия', 'moscow_oblast': 'Москва и Московская область',
    'murmansk_oblast': 'Мурманская область', 'nenets_autonomous_okrug': 'Ненецкий автономный округ',
    'nizhny_novgorod_oblast': 'Нижегородская область', 'north_ossetia_alania_republic': 'Северная Осетия',
    'novgorod_oblast': 'Новгородская область', 'novosibirsk_oblast': 'Новосибирская область',
    'omsk_oblast': 'Омская область', 'orenburg_oblast': 'Оренбургская область', 'oryol_oblast': 'Орловская область',
    'penza_oblast': 'Пензенская область', 'perm_krai': 'Пермский край', 'primorsky_krai': 'Приморский край',
    'pskov_oblast': 'Псковская область', 'rostov_oblast': 'Ростовская область', 'ryazan_oblast': 'Рязанская область',
    'sakha_republic': 'Якутия', 'sakhalin_oblast': 'Сахалинская область', 'samara_oblast': 'Самарская область',
    'saratov_oblast': 'Саратовская область', 'smolensk_oblast': 'Смоленская область',
    'stavropol_krai': 'Ставропольский край', 'sverdlovsk_oblast': 'Свердловская область',
    'tambov_oblast': 'Тамбовская область', 'tatarstan_republic': 'Татарстан', 'tomsk_oblast': 'Томская область',
    'tula_oblast': 'Тульская область', 'tuva_republic': 'Тыва', 'tver_oblast': 'Тверская область',
    'tyumen_oblast': 'Тюменская область', 'udmurt_republic': 'Удмуртия', 'ulyanovsk_oblast': 'Ульяновская область',
    'vladimir_oblast': 'Владимирская область', 'volgograd_oblast': 'Волгоградская область',
    'vologda_oblast': 'Вологодская область', 'voronezh_oblast': 'Воронежская область',
    'yamalo_nenets_autonomous_okrug': 'Ямало-Ненецкий автономный округ', 'yaroslavl_oblast': 'Ярославская область',
    'zabaykalsky_krai': 'Забайкальский край',
    # Ukraine's oblasts (the disputed lands: crimea, donetsk, luhansk above) and the CIS countries (2026-10-29); a
    # country by its regions - the regions' own names (as Russia's), a whole country - its name (the user 2026-10-29)
    'cherkasy_oblast': 'Черкасская область', 'chernihiv_oblast': 'Черниговская область',
    'chernivtsi_oblast': 'Черновицкая область', 'dnipropetrovsk_oblast': 'Днепропетровская область',
    'kharkiv_oblast': 'Харьковская область', 'kherson_oblast': 'Херсонская область',
    'khmelnytskyi_oblast': 'Хмельницкая область', 'kiev_oblast': 'Киев и Киевская область',
    'kirovohrad_oblast': 'Кировоградская область', 'lviv_oblast': 'Львовская область',
    'mykolaiv_oblast': 'Николаевская область', 'odessa_oblast': 'Одесская область',
    'poltava_oblast': 'Полтавская область', 'rivne_oblast': 'Ровенская область',
    'sumy_oblast': 'Сумская область', 'ternopil_oblast': 'Тернопольская область',
    'vinnytsia_oblast': 'Винницкая область', 'volyn_oblast': 'Волынская область',
    'zakarpattia_oblast': 'Закарпатская область', 'zaporizhia_oblast': 'Запорожская область',
    'zhytomyr_oblast': 'Житомирская область',
    'belarus': 'Беларусь', 'moldova': 'Молдова', 'georgia': 'Грузия', 'kazakhstan': 'Казахстан',
    'uzbekistan': 'Узбекистан', 'kyrgyzstan': 'Киргизия', 'tajikistan': 'Таджикистан', 'turkmenistan': 'Туркмения',
    'azerbaijan': 'Азербайджан', 'armenia': 'Армения',
}


def title(name):
    return TITLES.get(name, name)


def graph_bbox(path):
    """(minLon, minLat, maxLon, maxLat), degrees, from route.grf's header."""
    with open(path, 'rb') as f:
        h = struct.unpack(make_graph.HEADER, f.read(struct.calcsize(make_graph.HEADER)))
    min_lat, min_lon, max_lat, max_lon = h[6:10]
    return [min_lon / 1e6, min_lat / 1e6, max_lon / 1e6, max_lat / 1e6]


def region_info(path):
    """region.txt's fields: {name, date, source}."""
    with open(path, encoding='utf-8') as f:
        return dict(line.split('=', 1) for line in f.read().splitlines() if '=' in line)


def build_region(name, url, dist, work):
    """The region's zip and catalog entry in dist (its data built in work; no tiles: no planetiler there)."""
    folder = make_region.build(name, url, work, os.path.join(work, 'data'))
    os.makedirs(dist, exist_ok=True)
    zpath = os.path.join(dist, name + '.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in FILES:
            z.write(os.path.join(folder, f), f)
    entry = {'name': name, 'title': title(name), 'date': region_info(os.path.join(folder, 'region.txt'))['date'], 'zip': name + '.zip',
             'size': os.path.getsize(zpath), 'bbox': graph_bbox(os.path.join(folder, 'route.grf'))}
    with open(os.path.join(dist, name + '.json'), 'w', encoding='utf-8') as f:
        json.dump(entry, f)
    return entry


def build_day(build):
    """Protomaps' build's day: https://build.protomaps.com/20261030.pmtiles -> '20261030'."""
    return build.rsplit('/', 1)[1].split('.')[0]


def stored_zip(out, folder, files, info):
    """The folder's files into out, stored (unpacked by the unit piece by piece); region.txt: info."""
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
        for f in files:
            z.write(os.path.join(folder, f), f)
        z.writestr('region.txt', info)


def upload_zip(disk, name, path):
    """path up as DISK/name, gone here after (not into the workflow's artifacts); its size."""
    disk.upload('%s/%s' % (DISK, name), path)
    size = os.path.getsize(path)
    os.remove(path)
    return size


def build_full(entry, folder, dist, get_bytes, day, disk):
    """The region whole up (its tiles cut now), its entry told: full = {zip, size, tiles}."""
    tiles = os.path.join(folder, 'tiles.vtp')
    pmextract.extract(get_bytes, entry['bbox'], TILES_Z, tiles + '.part', None, pmextract.slim)
    os.replace(tiles + '.part', tiles)
    with open(os.path.join(folder, 'region.txt'), encoding='utf-8') as f:
        info = f.read() + 'tiles=%s\n' % day
    zpath = os.path.join(dist, entry['name'] + '.full.zip')
    stored_zip(zpath, folder, ('route.grf', 'search.idx', 'tiles.vtp'), info)
    entry['full'] = {'zip': entry['zip'], 'size': upload_zip(disk, entry['zip'], zpath), 'tiles': day}
    with open(os.path.join(dist, entry['name'] + '.json'), 'w', encoding='utf-8') as f:
        json.dump(entry, f)
    return entry


def publish_disk(dist, disk, get_bytes, day):
    """The overview whole up (maps_app's: z0-8 of all the regions' box), the folder public; the catalog in dist
    told: disk (the folder's public link), overview = {zip, size, tiles}."""
    import maps_app   # (its overview: one cut for the app and the release)
    path = os.path.join(dist, 'catalog.json')
    with open(path, encoding='utf-8') as f:
        cat = json.load(f)
    work = tempfile.mkdtemp()
    try:
        maps_app.install_overview(cat, work, get_bytes, day)
        folder = os.path.join(work, 'OSMapsData', maps_app.OVERVIEW)
        with open(os.path.join(folder, 'region.txt'), encoding='utf-8') as f:
            info = f.read()
        zpath = os.path.join(dist, 'overview.zip')
        stored_zip(zpath, folder, ('overview.vtp',), info)
        cat['overview'] = {'zip': 'overview.zip', 'size': upload_zip(disk, 'overview.zip', zpath), 'tiles': day}
    finally:
        shutil.rmtree(work, ignore_errors=True)
    cat['disk'] = disk.publish(DISK)
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(cat, f, ensure_ascii=False)
    return cat


def kept(previous, built, known):
    """The previous catalog's entries not built now, still among the regions; with today's title (the user
    2026-10-29: "Украина:" off the oblasts - not only those rebuilt)."""
    out = [e for e in (previous or {'regions': []})['regions'] if e['name'] not in built and e['name'] in known]
    for e in out:
        if 'title' in e:
            e['title'] = title(e['name'])
    return out


def catalog(dist, previous=None, known=()):
    """catalog.json of the entries in dist and the previous catalog's others still among the regions (known), by
    name."""
    entries = []
    for f in sorted(os.listdir(dist)):
        if f.endswith('.json') and f != 'catalog.json':
            with open(os.path.join(dist, f), encoding='utf-8') as e:
                entries.append(json.load(e))
    built = {e['name'] for e in entries}
    entries += kept(previous, built, known)
    cat = {'version': 1, 'made': time.strftime('%Y-%m-%d', time.gmtime()),
           'regions': sorted(entries, key=lambda e: e['name'])}
    for k in ('disk', 'overview'):   # (a run without the disk: the previous one's still there)
        if previous and k in previous:
            cat[k] = previous[k]
    with open(os.path.join(dist, 'catalog.json'), 'w', encoding='utf-8') as f:
        json.dump(cat, f, ensure_ascii=False)
    return cat


def main(argv):
    if argv[0] == 'list':
        print(json.dumps(sorted(make_region.regions())))
    elif argv[0] == 'build':
        entry = build_region(argv[1], make_region.regions()[argv[1]], argv[2], make_region.WORK)
        if os.environ.get('YANDEX_DISK_TOKEN'):
            build = pmextract.latest_build()
            entry = build_full(entry, os.path.join(make_region.WORK, 'data', argv[1]), argv[2],
                               pmextract.http_bytes(build), build_day(build), yadisk.Disk(os.environ['YANDEX_DISK_TOKEN']))
        print(entry)
    elif argv[0] == 'disk':
        build = pmextract.latest_build()
        publish_disk(argv[1], yadisk.Disk(os.environ['YANDEX_DISK_TOKEN']), pmextract.http_bytes(build),
                     build_day(build))
    else:
        catalog(argv[1], json.load(open(argv[2], encoding='utf-8')) if len(argv) > 2 else None, make_region.regions())


if __name__ == '__main__':
    main(sys.argv[1:])
