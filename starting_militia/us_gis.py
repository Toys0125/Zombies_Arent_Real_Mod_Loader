"""Build compact, offline parcel-use caches for the installed U.S. maps."""

import argparse
import gzip
import json
import math
import os
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from itertools import islice
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from sf_gis import MAP_HEADER, MAP_NAME, MOD_DIR, build_cache, read_cache


CACHE_DIR = MOD_DIR / "gis"

# Each source identifies existing or assessed use. Mixed use, vacancies, and
# classes that do not cleanly mean home/shop/office are left to the game.
SOURCES = {
    "boston": ("custom_us_boston_d13ba0.map",
               "https://gis.bostonplans.org/hosting/rest/services/FY26_Parcels_Detailed/MapServer/0",
               "LU", {"R1", "R2", "R3", "R4", "A", "CD"}, {"C", "CC"}),
    "detroit": ("custom_us_detroit_1da3f1.map",
                "https://services2.arcgis.com/qvkbeam7Wirps6zC/arcgis/rest/services/Parcels_Current/FeatureServer/0",
                "property_class", {"401", "407"}, {"201", "207"}),
    "new_york": ("ny.map",
                 "https://services5.arcgis.com/GfwWNkhOj9bNBqoJ/arcgis/rest/services/MAPPLUTO/FeatureServer/0",
                 "LandUse", {"1", "2", "3"}, {"5"}),
    "miami": ("miami_florida.map",
              "https://gisweb.miamidade.gov/ArcGIS/rest/services/MD_Communications/MapServer/2",
              "LU", {"10", "11", "12", "13", "20", "30", "35", "61", "65", "69"},
              {"101", "110", "113", "200"}),
    "seattle": ("seattle_wa.map",
                "https://services.arcgis.com/ZOyb2t4B0UYuYNYH/arcgis/rest/services/Existing_Land_Use_(20-category)/FeatureServer/0",
                "LU_PLN_DESC", {"SINGLE FAMILY", "DUPLEX/TRIPLEX", "MULTI-FAMILY", "OTHER HOUSING"},
                {"HOTEL/MOTEL", "OFFICE", "RETAIL/SERVICE"}),
    # The undissolved 2020 inventory preserves more usable boundaries here than the 2023 view.
    "chicago": ("chicago_il.map",
                "https://services5.arcgis.com/LcMXE3TFhi1BSaCY/arcgis/rest/services/LUI20_geodatabase_v1_CMAP/FeatureServer/0",
                "LANDUSE", {"1111", "1112", "1130", "1140"}, {"1211", "1212", "1214", "1215", "1220", "1250"}),
    "washington_dc": ("custom_us_capitolhill_d15788.map",
                      "https://maps2.dcgis.dc.gov/DCGIS/rest/services/DCGIS_DATA/Planning_Landuse_and_Zoning_WebMercator/MapServer/4",
                      "PRIMARYLU", {"RESIDENTIAL"}, {"COMMERCIAL"}),
}


def get_json(url, parameters):
    request = Request(f"{url}?{urlencode(parameters, safe=',')}", headers={"Accept-Encoding": "gzip"})
    for attempt in range(3):
        try:
            with urlopen(request, timeout=40) as response:
                if response.headers.get("Content-Encoding") == "gzip":
                    with gzip.GzipFile(fileobj=response) as stream:
                        result = json.load(stream)
                else:
                    result = json.load(response)
            if "error" in result:
                raise ValueError(result["error"])
            return result
        except (OSError, ValueError) as error:
            if isinstance(error, HTTPError) and error.code in (404, 414):
                raise
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def parcel_features(url, field, bounds):
    layer = get_json(url, {"f": "json"})
    oid = next(f["name"] for f in layer["fields"] if f["type"] == "esriFieldTypeOID")
    record_limit = layer["maxRecordCount"]
    spatial = {
        "where": "1=1",
        "geometry": ",".join(map(str, bounds)),
        "geometryType": "esriGeometryEnvelope",
        "inSR": 4326,
        "spatialRel": "esriSpatialRelIntersects",
    }
    query = url + "/query"
    ids = sorted(set(get_json(query, spatial | {"f": "json", "returnIdsOnly": "true"})["objectIds"]))
    print(f"Reading {len(ids):,} land-use records from {layer['name']}...", flush=True)

    def page(object_ids):
        contiguous_range = object_ids[-1] - object_ids[0] + 1 <= record_limit
        selector = ({"where": f"{oid} BETWEEN {object_ids[0]} AND {object_ids[-1]}"} if contiguous_range
                    else {"objectIds": ",".join(map(str, object_ids))})
        try:
            result = get_json(query, {
                "f": "geojson", "outSR": 4326, "outFields": f"{oid},{field}", "geometryPrecision": 7,
            } | selector)
        except (HTTPError, TimeoutError) as error:
            if len(object_ids) < 2 or (isinstance(error, HTTPError) and error.code not in (404, 414, 500, 502, 503, 504)):
                raise
            middle = len(object_ids) // 2
            return page(object_ids[:middle]) + page(object_ids[middle:])
        expected = set(object_ids)
        features = [feature for feature in result["features"] if int(feature["properties"][oid]) in expected]
        received = {int(feature["properties"][oid]) for feature in features}
        if len(received) != len(features):
            raise ValueError(f"Unexpected parcel IDs from {layer['name']}.")
        missing = expected - received
        if missing:
            # A layer can omit null polygons from GeoJSON. Verify the missing
            # records separately so incomplete downloads can't replace a cache.
            raw = get_json(query, {
                "f": "json", "outFields": oid, "outSR": 4326,
                "objectIds": ",".join(map(str, sorted(missing))),
            })["features"]
            if {int(item["attributes"][oid]) for item in raw if not item.get("geometry")} != missing:
                raise ValueError(f"Incomplete GIS page from {layer['name']}.")
        return features

    # Dense ID ranges make short queries even at the server's record limit.
    # Use short lists for sparse ranges so request URLs fit service limits.
    def pages():
        start = 0
        while start < len(ids):
            end = min(start + record_limit, len(ids))
            if ids[end - 1] - ids[start] >= record_limit:
                end = min(start + 400, len(ids))
            yield ids[start:end]
            start = end

    outstanding = pages()
    checked = 0
    next_update = 50_000
    with ThreadPoolExecutor(max_workers=8) as executor:
        while batch := list(islice(outstanding, 8)):
            for features in executor.map(page, batch):
                yield from features
            checked += sum(map(len, batch))
            if checked >= next_update:
                print(f"{layer['name']}: checked {checked:,} of {len(ids):,} land-use records.", flush=True)
                next_update += 50_000


def saved_lookup(game_dir, map_name):
    return read_cache(game_dir, map_name, CACHE_DIR / Path(map_name).with_suffix(".bin").name)


def read_catalog(game_dir):
    sources = [(MAP_NAME, read_cache(game_dir))]
    sources.extend((entry[0], saved_lookup(game_dir, entry[0])) for entry in SOURCES.values())
    return [(name, count, payload) for name, (count, payload) in sources if count]


def refresh(city):
    map_name, url, field, homes, commerce = SOURCES[city]
    map_bytes = (MOD_DIR.parent / "maps" / map_name).read_bytes()
    _, _, width, height, lon, lat, mx, my, georef, _ = MAP_HEADER.unpack_from(map_bytes)
    if not all(map(math.isfinite, (width, height, lon, lat, mx, my))) or min(width, height, mx, my) <= 0:
        raise ValueError(f"Invalid map bounds for {map_name}.")
    if not georef:
        # The legacy NYC file has the coordinates but does not set the flag.
        campaign = MOD_DIR.parent / "campaigns/New_York_City.cmp"
        fields = dict(line.split("=", 1) for line in campaign.read_text().splitlines() if "=" in line)
        reference = {key.strip(): value.strip() for key, value in fields.items()}
        if city != "new_york" or any(
            not math.isclose(float(reference[key]), value, rel_tol=0, abs_tol=1e-5)
            for key, value in zip(("origin_lon", "origin_lat", "map_mx", "map_my"), (lon, lat, mx, my))
        ):
            raise ValueError(f"Unverified map projection for {map_name}.")

    def classify(properties):
        use = str(properties.get(field)).strip().upper().lstrip("0")
        return 1 if use in homes else 2 if use in commerce else 0

    bounds = (lon, lat, lon + width / mx, lat + height / my)
    encoded = build_cache(map_bytes, parcel_features(url, field, bounds), classify, allow_unmarked_georef=not georef)
    CACHE_DIR.mkdir(exist_ok=True)
    archive = CACHE_DIR / Path(map_name).with_suffix(".bin").name
    with tempfile.TemporaryDirectory(prefix="gis-", dir=CACHE_DIR) as temporary:
        update = Path(temporary) / archive.name
        update.write_bytes(encoded)
        os.replace(update, archive)
    print(f"Saved {len(encoded):,} bytes for {city}.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("city", choices=SOURCES)
    args = parser.parse_args()
    refresh(args.city)


if __name__ == "__main__":
    main()
