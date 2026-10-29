"""The regions' offline data for GitHub Releases (the user 2026-10-27: the graph and the search there, built by
Actions - .github/workflows/maps.yml; the tiles are not ours: the PC app cuts them out of Protomaps' build).

    python release_regions.py list                 the regions' names, JSON (the workflow's matrix)
    python release_regions.py build <name> <dist>  <dist>/<name>.zip (route.grf, search.idx, region.txt) and
                                                   <dist>/<name>.json (its catalog entry)
    python release_regions.py catalog <dist> [previous.json]
                                                   <dist>/catalog.json of the entries there, the previous catalog's
                                                   regions not built this time kept (their zips stay in the release)

A catalog entry: name, date (the OSM extract's), zip, size, bbox (minLon, minLat, maxLon, maxLat: the road graph's
bounds, for the tiles' cut). The release: the tag "maps" (a pre-release: never the program's latest), its assets
replaced on each run - the app reads ASSETS + "catalog.json".
"""
import json
import os
import struct
import sys
import time
import zipfile

import make_graph
import make_region

ASSETS = 'https://github.com/TpuPyku/OSMapsCE/releases/download/maps/'
FILES = ('route.grf', 'search.idx', 'region.txt')


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
    # Ukraine's oblasts (the disputed lands: crimea, donetsk, luhansk above) and the CIS countries (2026-10-29)
    'cherkasy_oblast': 'Украина: Черкасская область', 'chernihiv_oblast': 'Украина: Черниговская область',
    'chernivtsi_oblast': 'Украина: Черновицкая область', 'dnipropetrovsk_oblast': 'Украина: Днепропетровская область',
    'kharkiv_oblast': 'Украина: Харьковская область', 'kherson_oblast': 'Украина: Херсонская область',
    'khmelnytskyi_oblast': 'Украина: Хмельницкая область', 'kiev_oblast': 'Украина: Киев и Киевская область',
    'kirovohrad_oblast': 'Украина: Кировоградская область', 'lviv_oblast': 'Украина: Львовская область',
    'mykolaiv_oblast': 'Украина: Николаевская область', 'odessa_oblast': 'Украина: Одесская область',
    'poltava_oblast': 'Украина: Полтавская область', 'rivne_oblast': 'Украина: Ровенская область',
    'sumy_oblast': 'Украина: Сумская область', 'ternopil_oblast': 'Украина: Тернопольская область',
    'vinnytsia_oblast': 'Украина: Винницкая область', 'volyn_oblast': 'Украина: Волынская область',
    'zakarpattia_oblast': 'Украина: Закарпатская область', 'zaporizhia_oblast': 'Украина: Запорожская область',
    'zhytomyr_oblast': 'Украина: Житомирская область',
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


def catalog(dist, previous=None, known=()):
    """catalog.json of the entries in dist and the previous catalog's others still among the regions (known), by
    name."""
    entries = []
    for f in sorted(os.listdir(dist)):
        if f.endswith('.json') and f != 'catalog.json':
            with open(os.path.join(dist, f), encoding='utf-8') as e:
                entries.append(json.load(e))
    built = {e['name'] for e in entries}
    entries += [e for e in (previous or {'regions': []})['regions'] if e['name'] not in built and e['name'] in known]
    cat = {'version': 1, 'made': time.strftime('%Y-%m-%d', time.gmtime()),
           'regions': sorted(entries, key=lambda e: e['name'])}
    with open(os.path.join(dist, 'catalog.json'), 'w', encoding='utf-8') as f:
        json.dump(cat, f, ensure_ascii=False)
    return cat


def main(argv):
    if argv[0] == 'list':
        print(json.dumps(sorted(make_region.regions())))
    elif argv[0] == 'build':
        print(build_region(argv[1], make_region.regions()[argv[1]], argv[2], make_region.WORK))
    else:
        catalog(argv[1], json.load(open(argv[2], encoding='utf-8')) if len(argv) > 2 else None, make_region.regions())


if __name__ == '__main__':
    main(sys.argv[1:])
