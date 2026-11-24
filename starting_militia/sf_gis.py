"""Refresh the compact San Francisco land-use lookup from DataSF."""

import gzip
import hashlib
import json
import math
import os
import struct
import sys
import tempfile
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import Request, urlopen


MOD_DIR = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
MAP_NAME = "custom_us_sanfrancisco_c76781.map"
CACHE_PATH = MOD_DIR / "san_francisco_gis.bin"
PRESIDIO_HOUSING = MOD_DIR / "presidio_housing.json"
DATA_URL = "https://data.sf.gov/resource/c5ge-t6pj"
FIELDS = "the_geom,resunits,total_comm,mips,retail,visitor,garage,parking_lo,open_space,geography_type"
HEADER = struct.Struct("<4s32sI")
# v0.44.0 rewrote these maps as NAM1 v13. Every cached coordinate key
# still resolves to exactly one footprint in each verified map.
MAP_HASH_VARIANTS = {
    "custom_us_boston_d13ba0.map": (
        "ea63c4217ce2bc4d73b3b6a7a370add089e22179611c442004b10f6a47e5d3f7",
        "7340e879cc55f99cd61bb0e253da2767cb150a5ab1bcc33a6973513eaa7cf6ac",
    ),
    "custom_us_detroit_1da3f1.map": (
        "7110e55878a94f115c47e3a2ecab2a266387f87efe854804cd7abdc6fd65a0c5",
        "45ede60ecc3a89123de13d5539a6ff0294727d835c510f3b2ab044f51026dd31",
    ),
    "ny.map": (
        "e83f1ad42f3eaadf6d1f76b103fc9a54e38bbc7e0a3b1682736f0d62d67f46ea",
        "1d0c1b77d1b7fa64d6995e4ff1dda95364e4f47e8aac613d586eadc6801812b1",
    ),
    "miami_florida.map": (
        "f5739c6cd8fd31ccfd8edd9287d31525b48ec7ff5efbab13dc835104c3b06562",
        "f0c7c676b8b4032f05f89987209c21f38a701d3da0962865bcf8552c73789c28",
    ),
    "seattle_wa.map": (
        "f9ba29cd6ffba9b49fa3076e0c29c7b65eed5ba0a34dd98983939f9a583328d2",
        "399112f7e1fb80114c34b5b85c80c3b5c0041bee87c89df77d2706b7a2d730d0",
    ),
    "chicago_il.map": (
        "3436dcbf53386d845aaf99aeccbc92dd92675d3f7af6a94b6b16d92d36ed697d",
        "af11b3be1b898522b4693ce30689cbd68734b952ca7de702b9e6aeec2fd29803",
    ),
    "custom_us_capitolhill_d15788.map": (
        "6abda94f78888f567380aac2d50216d7a05cd5a7c0d6272714a3393e35887ad2",
        "f660f83096772fce659ed34273a18f038b9c8d0fcad1f6fe3cb69ae27417347f",
    ),
}
MAP_HEADER = struct.Struct("<4sI2f4d2I")
BUILDING = struct.Struct("<4Q7f3I")
POINT = struct.Struct("<2f")
PAGE_SIZE = 10_000
CELL_SIZE = 64


def request(format_name, parameters):
    url = f"{DATA_URL}.{format_name}?{urlencode(parameters)}"
    query = Request(url, headers={"Accept-Encoding": "gzip"})
    for attempt in range(3):
        try:
            with urlopen(query, timeout=55) as response:
                if response.headers.get("Content-Encoding") == "gzip":
                    with gzip.GzipFile(fileobj=response) as stream:
                        return json.load(stream)
                return json.load(response)
        except OSError:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def datasf_features():
    meta = request("json", {"$select": "count(*) as records, max(data_as_of) as vintage"})[0]
    count, vintage = int(meta["records"]), meta["vintage"]
    print(f"Reading {count:,} DataSF land-use areas from {vintage[:10]}...", flush=True)

    def page(offset):
        result = request("geojson", {
            "$select": FIELDS + ",ludb_id", "$where": f"data_as_of = '{vintage}'",
            "$order": "ludb_id", "$limit": PAGE_SIZE, "$offset": offset,
        })["features"]
        if len(result) != min(PAGE_SIZE, count - offset):
            raise ValueError("DataSF returned an incomplete snapshot. Try again after the dataset finishes updating.")
        return result

    # Bound concurrent requests and discard each page after joining its polygons.
    with ThreadPoolExecutor(max_workers=4) as executor:
        for start in range(0, count, 4 * PAGE_SIZE):
            offsets = range(start, min(count, start + 4 * PAGE_SIZE), PAGE_SIZE)
            for features in executor.map(page, offsets):
                yield from features


def building_key(x, y):
    # Half-meter cells, with a gap near rounding boundaries to tolerate the
    # game's single-precision translation when it crops a campaign stage.
    sx, sy = x * 2, y * 2
    qx, qy = math.floor(sx + 0.5), math.floor(sy + 0.5)
    if 0 <= qx <= 65535 and 0 <= qy <= 65535 and abs(sx - qx) < 0.49 and abs(sy - qy) < 0.49:
        return (qx << 16) | qy
    return None


def parcel_use(properties):
    if properties.get("open_space") or properties.get("parking_lo") or properties.get("garage"):
        return 0
    if properties.get("geography_type") == "multiple_parcels":
        return 0
    homes = float(properties.get("resunits") or 0)
    commercial = float(properties.get("total_comm") or 0)
    if homes > 0 and commercial == 0:
        return 1
    if homes == 0 and commercial > 0:
        storefront_or_office = sum(float(properties.get(field) or 0) for field in ("retail", "mips", "visitor"))
        if storefront_or_office >= 0.8 * commercial:
            return 2
    return 0


def inside_ring(x, y, ring):
    inside = False
    px, py = ring[-1]
    for nx, ny in ring:
        if (ny > y) != (py > y) and x < px + (y - py) * (nx - px) / (ny - py):
            inside = not inside
        px, py = nx, ny
    return inside


def inside_polygon(point, rings):
    return inside_ring(*point, rings[0]) and not any(inside_ring(*point, hole) for hole in rings[1:])


def build_cache(map_bytes, features, classify=parcel_use, *, allow_unmarked_georef=False):
    magic, version, _, _, lon, lat, mx, my, georef, vertex_count = MAP_HEADER.unpack_from(map_bytes)
    if (magic != b"NAM1" or version not in (12, 13) or not (georef == 1 or allow_unmarked_georef)
            or not (mx > 0 and my > 0)):
        raise ValueError("The map does not have the supported georeferenced map layout.")

    vertex_start = MAP_HEADER.size
    building_count_offset = vertex_start + vertex_count * POINT.size
    (building_count,) = struct.unpack_from("<I", map_bytes, building_count_offset)
    building_start = building_count_offset + 4
    view = memoryview(map_bytes)[building_start:building_start + building_count * BUILDING.size]
    if len(view) != building_count * BUILDING.size:
        raise ValueError("Incomplete map building records.")

    buildings, grid = [], defaultdict(list)
    keys, duplicate_keys = set(), set()
    for index, (offset, length, _, _, _, _, lo_x, lo_y, hi_x, hi_y, area, *_) in enumerate(BUILDING.iter_unpack(view)):
        x, y = (lo_x + hi_x) * 0.5, (lo_y + hi_y) * 0.5
        key = building_key(x, y)
        buildings.append((x, y, offset, length, area, key))
        if key is None:
            continue
        if key in keys:
            duplicate_keys.add(key)
        keys.add(key)
        if area >= 40 and length >= 3 and offset + length <= vertex_count:
            grid[(int(x // CELL_SIZE), int(y // CELL_SIZE))].append(index)

    if not grid:
        raise ValueError("The map has no eligible building footprints.")
    columns, rows = zip(*grid)
    first_column, last_column = min(columns), max(columns)
    first_row, last_row = min(rows), max(rows)
    claims, uses = bytearray(building_count), bytearray(building_count)
    feature_count = 0
    for feature in features:
        feature_count += 1
        geometry = feature.get("geometry")
        if not geometry:
            continue
        coords = geometry["coordinates"]
        if geometry["type"] == "Polygon":
            coords = [coords]
        elif geometry["type"] != "MultiPolygon":
            continue
        use = classify(feature["properties"])
        seen = set()
        for polygon in coords:
            rings = [[((x - lon) * mx, (y - lat) * my) for x, y, *_ in ring] for ring in polygon if ring]
            if not rings:
                continue
            xs, ys = zip(*rings[0])
            min_x, max_x, min_y, max_y = min(xs), max(xs), min(ys), max(ys)
            column_start = max(first_column, int(min_x // CELL_SIZE))
            column_end = min(last_column, int(max_x // CELL_SIZE))
            row_start = max(first_row, int(min_y // CELL_SIZE))
            row_end = min(last_row, int(max_y // CELL_SIZE))
            for cell_y in range(row_start, row_end + 1):
                for cell_x in range(column_start, column_end + 1):
                    for index in grid.get((cell_x, cell_y), ()):
                        x, y, offset, length, _, _ = buildings[index]
                        if index in seen or not (min_x <= x <= max_x and min_y <= y <= max_y):
                            continue
                        if not inside_polygon((x, y), rings):
                            continue
                        seen.add(index)
                        if claims[index]:
                            uses[index] = 0
                            continue
                        claims[index] = 1
                        if not use:
                            continue

                        # Check inset points toward the actual footprint extrema.
                        # This drops structures that straddle different parcels.
                        start = vertex_start + offset * POINT.size
                        points = list(POINT.iter_unpack(memoryview(map_bytes)[start:start + length * POINT.size]))
                        for axis in (0, 1):
                            low = min(points, key=lambda p: p[axis])
                            high = max(points, key=lambda p: p[axis])
                            if not all(inside_polygon((x + 0.65 * (px - x), y + 0.65 * (py - y)), rings)
                                       for px, py in (low, high)):
                                break
                        else:
                            uses[index] = use

    entries = sorted((building[-1] * 2 + use - 1)
                     for building, use in zip(buildings, uses) if use and building[-1] not in duplicate_keys)
    if not entries:
        raise ValueError("No building footprints matched the GIS data.")

    counts = Counter((entry & 1) + 1 for entry in entries)
    print(f"Matched {counts[1]:,} residential and {counts[2]:,} commercial footprints "
          f"from {feature_count:,} GIS records.", flush=True)
    return encode_cache(hashlib.sha256(map_bytes).digest(), entries)


def encode_cache(map_hash, entries):
    encoded = bytearray(HEADER.pack(b"SFG1", map_hash, len(entries)))
    # Adjacent coordinates usually differ by only a few bits. Delta encoding
    # keeps the lookup small without storing the source parcel polygons.
    previous = 0
    for entry in entries:
        delta = entry - previous
        previous = entry
        while delta >= 128:
            encoded.append((delta & 127) | 128)
            delta >>= 7
        encoded.append(delta)

    return encoded


def cache_entries(cache):
    entry = delta = shift = 0
    for byte in memoryview(cache)[HEADER.size:]:
        delta |= (byte & 127) << shift
        if byte & 128:
            shift += 7
            continue
        entry += delta
        yield entry
        delta = shift = 0


def add_presidio_housing(map_bytes, city_cache):
    # Public Presidio Trust housing areas, aligned to the game footprints.
    # Fill gaps in the parcel lookup and retain any existing parcel use.
    _, map_hash, count = HEADER.unpack_from(city_cache)
    if map_hash != hashlib.sha256(map_bytes).digest():
        raise ValueError("The San Francisco lookup belongs to a different map.")
    areas = json.loads(PRESIDIO_HOUSING.read_text(encoding="utf-8"))
    features = ({"properties": {"resunits": 1},
                 "geometry": {"type": "Polygon", "coordinates": [ring]}} for _, ring in areas)
    additions = build_cache(map_bytes, features)
    uses = {entry >> 1: entry for entry in cache_entries(additions)}
    uses.update((entry >> 1, entry) for entry in cache_entries(city_cache))
    print(f"Filled {len(uses) - count:,} gaps with Presidio housing data.", flush=True)
    return encode_cache(map_hash, sorted(uses.values()))


def read_cache(game_dir, map_name=MAP_NAME, cache_path=CACHE_PATH):
    if not cache_path.is_file():
        return 0, b""
    map_path = game_dir / "maps" / map_name
    if not map_path.is_file():
        return 0, b""
    cache = cache_path.read_bytes()
    if len(cache) < HEADER.size:
        raise ValueError(f"The GIS lookup {cache_path.name} is incomplete.")
    magic, map_hash, count = HEADER.unpack_from(cache)
    if magic != b"SFG1" or not (count <= len(cache) - HEADER.size <= count * 5):
        raise ValueError(f"The GIS lookup {cache_path.name} is invalid.")
    installed_hash = hashlib.sha256(map_path.read_bytes()).digest()
    verified_variant = MAP_HASH_VARIANTS.get(map_name) == (map_hash.hex(), installed_hash.hex())
    if installed_hash != map_hash and not verified_variant:
        print(f"{map_name} changed. Skipping its stale GIS lookup.", flush=True)
        return 0, b""
    return count, cache[HEADER.size:]


def main():
    map_bytes = (MOD_DIR.parent / "maps" / MAP_NAME).read_bytes()
    encoded = add_presidio_housing(map_bytes, build_cache(map_bytes, datasf_features()))
    with tempfile.TemporaryDirectory(prefix="sf-gis-", dir=MOD_DIR) as temporary:
        update = Path(temporary) / CACHE_PATH.name
        update.write_bytes(encoded)
        os.replace(update, CACHE_PATH)
    print(f"Saved {len(encoded):,} bytes to {CACHE_PATH.name}.")


if __name__ == "__main__":
    main()
