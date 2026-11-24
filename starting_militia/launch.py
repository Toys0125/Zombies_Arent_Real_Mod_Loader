"""Build and launch Armed Citizens Mod for Zombies Aren't Real."""

import argparse
import configparser
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from decimal import Decimal, InvalidOperation

from game_builds import SUPPORTED_BUILDS
from sf_gis import MOD_DIR

GAME_DIR = MOD_DIR.parent
STOCK_EXE = GAME_DIR / "zombies-arent-real.exe"
MODDED_EXE = GAME_DIR / "zombies-arent-real-militia.exe"
US_MAP_IDS = (b"chicago_il", b"miami_florida", b"ny", b"seattle_wa")
# Keep the threshold order aligned with the building-use IDs in the hook.
CHANCE_DEFAULTS = {"other": "0.25", "residential": "4", "commercial": "0.025"}

MENU_SETTINGS = (
    "residential_chance_percent", "commercial_chance_percent", "other_chance_percent",
    "allow_all_maps", "allow_coop", "extra_us_map_ids", "enabled",
)
SETTINGS_HOOKS = ("layout_mod_settings", "draw_mod_settings", "action_mod_settings", "input_mod_settings")

STATE_OFFSETS = {
    "count_militia_this_battle": 0,
    "inferred_building_uses": 8,
    "inferred_building_count": 16,
    "inferred_gis_map": 24,
    "active_militia_rules": 32,
    "militia_coop_ready_seed": 40,
    "militia_guest_rules": 48,
}
COOP_RULES_CAPACITY = 4096  # Keep in sync with coop_rules.c.
STATE_OFFSETS["view_button_scratch"] = STATE_OFFSETS["militia_guest_rules"] + COOP_RULES_CAPACITY
STATE_OFFSETS["militia_qualified"] = STATE_OFFSETS["view_button_scratch"] + 4 * 0x58
STATE_OFFSETS["militia_spawn_info_ready"] = STATE_OFFSETS["militia_qualified"] + 3 * 8
STATE_OFFSETS["militia_spawn_lines"] = STATE_OFFSETS["militia_spawn_info_ready"] + 1
STATE_OFFSETS["militia_preview_positions"] = (STATE_OFFSETS["militia_spawn_lines"] + 4 * 48 + 7) & -8
STATE_OFFSETS["militia_preview_count"] = STATE_OFFSETS["militia_preview_positions"] + 8
STATE_OFFSETS["militia_preview_profile"] = STATE_OFFSETS["militia_preview_count"] + 8
STATE_OFFSETS["mod_settings_menu"] = STATE_OFFSETS["militia_preview_profile"] + 8
STATE_SIZE = STATE_OFFSETS["mod_settings_menu"] + 0x2400

def align(value, boundary):
    return (value + boundary - 1) // boundary * boundary


def read_settings(path):
    settings = configparser.ConfigParser(interpolation=None)
    with path.open(encoding="utf-8-sig") as file:
        settings.read_file(file)
    return parse_settings(settings["militia"])


def parse_settings(militia):
    chances = {}
    for building_use, default in CHANCE_DEFAULTS.items():
        option = f"{building_use}_chance_percent"
        chance = Decimal(militia.get(option, militia.get("chance_percent", default)))
        if not chance.is_finite() or not 0 <= chance <= 100:
            raise ValueError(f"{option} must be a number from 0 to 100.")
        chances[building_use] = chance
    allow_all_maps = militia.getboolean("allow_all_maps", fallback=False)
    map_ids = list(US_MAP_IDS)
    for name in militia.get("extra_us_map_ids", "").split(","):
        name = name.strip()
        if not name:
            continue
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", name):
            raise ValueError(f"Invalid map ID {name!r}. Use the file name without .map.")
        map_ids.append(name.encode("ascii"))
    entries = bytearray()
    for name in dict.fromkeys(map_ids):
        entries.append(len(name))
        entries.extend(name)
    entries.append(0)
    thresholds = tuple(int(chance * (1 << 32) / 100) for chance in chances.values())
    rules = struct.pack("<3QB", *thresholds, allow_all_maps) + entries
    return chances, rules, militia.getboolean("allow_coop", fallback=False), militia.getboolean("enabled", fallback=True)


def save_menu_settings(path, values):
    # A game opened before an update still sends the original six values.
    if len(values) not in (len(MENU_SETTINGS) - 1, len(MENU_SETTINGS)) or any(
        value != "-" and not value.startswith("+") for value in values
    ):
        raise ValueError("Invalid input from the Mods menu.")
    changes = {key: value[1:] for key, value in zip(MENU_SETTINGS, values) if value != "-"}
    original = path.read_text(encoding="utf-8-sig")
    settings = configparser.ConfigParser(interpolation=None)
    settings.read_string(original)
    settings["militia"].update(changes)
    _, rules, allow_coop, enabled = parse_settings(settings["militia"])
    if enabled and allow_coop and len(rules) > COOP_RULES_CAPACITY:
        raise ValueError("The co-op map list is too long. Remove some extra U.S. map IDs.")

    # Keep comments and formatting in a standard INI. Validate the result
    # against ConfigParser before using it, including for continued values.
    edited = original
    for key, value in changes.items():
        edited, replacements = re.subn(
            rf"^([ \t]*{key}[ \t]*[=:][ \t]*)[^\n]*$",
            lambda match: match[1] + value, edited, count=1, flags=re.I | re.M,
        )
        if not replacements:
            edited = re.sub(r"^(\[militia\][^\n]*\n)", lambda match: f"{match[1]}{key} = {value}\n",
                            edited, count=1, flags=re.M)
    check = configparser.ConfigParser(interpolation=None)
    try:
        check.read_string(edited)
    except configparser.Error:
        check.clear()
    if check != settings:
        output = io.StringIO()
        settings.write(output)
        edited = output.getvalue()

    with tempfile.TemporaryDirectory(prefix="settings-", dir=path.parent) as directory:
        temporary = Path(directory) / path.name
        temporary.write_text(edited, encoding="utf-8")
        os.replace(temporary, path)


def menu_settings_data(rules, allow_coop, chances, enabled):
    thresholds = struct.unpack_from("<3Q", rules)
    if chances is None:
        chances = {key: Decimal(n) * 100 / (1 << 32) for key, n in zip(CHANCE_DEFAULTS, thresholds)}
    values = [str(chances[key]) for key in ("residential", "commercial", "other")]
    values.extend(("true" if rules[24] else "false", "true" if allow_coop else "false"))
    extra = []
    offset = 25
    while length := rules[offset]:
        name = rules[offset + 1:offset + 1 + length]
        if name not in US_MAP_IDS:
            extra.append(name.decode("ascii"))
        offset += length + 1
    values.append(", ".join(extra))
    values.append("true" if enabled else "false")
    return ("\0".join(values) + "\0").encode("ascii")


C_OBJECT_NAMES = ("inferred_building_uses.obj", "coop_rules.obj", "spawn_info.obj", "mod_settings.obj")


def compile_objects(directory, build):
    clang = shutil.which("clang")
    if not clang:
        clang = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM/bin/clang.exe"
        if not clang.is_file():
            raise ValueError("LLVM clang was not found. Install LLVM or add clang to PATH.")
    target = [clang, "--target=x86_64-pc-windows-msvc"]
    assembly = directory / f"starting_militia-{build.version}.obj"
    c_objects = tuple(directory / name for name in C_OBJECT_NAMES)
    defines = ["-Wa,-defsym,STEAM_V044=1"] if build.version == "0.44.0" else []
    subprocess.run(
        [*target, *defines, "-I", MOD_DIR, "-c", MOD_DIR / "starting_militia.s", "-o", assembly],
        check=True,
    )
    for obj in c_objects:
        if obj.is_file():
            continue  # Shared by both assembly builds during packaging.
        subprocess.run(
            [*target, "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-stack-protector",
             "-fno-builtin", "-fno-math-errno", "-funwind-tables", "-fno-addrsig",
             "-fno-vectorize", "-fno-slp-vectorize",
             "-c", MOD_DIR / obj.with_suffix(".c").name, "-o", obj],
            check=True,
        )
    return assembly, *c_objects


def assemble(directory, build):
    objects = (
        [Path(sys._MEIPASS) / name for name in (f"starting_militia-{build.version}.obj", *C_OBJECT_NAMES)]
        if getattr(sys, "frozen", False)
        else compile_objects(directory, build)
    )
    code, labels, relocs, pdata_ranges = bytearray(), {}, [], []
    for obj_path in objects:
        append_coff_object(obj_path.read_bytes(), code, labels, relocs, pdata_ranges)
    return code, labels, relocs, pdata_ranges


def append_coff_object(obj, code, labels, relocs, pdata_ranges):
    machine, section_count, _, symbol_table, symbol_count, optional_size, _ = struct.unpack_from(
        "<HHIIIHH", obj
    )
    if machine != 0x8664 or optional_size != 0:
        raise ValueError("Expected a Windows x64 COFF object from clang.")
    sections = [
        struct.unpack_from("<8sIIIIIIHHI", obj, 20 + 40 * index)
        for index in range(section_count)
    ]
    section_offsets = {}
    for number, section in enumerate(sections, 1):
        raw_name, _, _, size, offset, _, _, _, _, flags = section
        if not size:
            continue
        name = raw_name.rstrip(b"\0")
        if name not in (b".text", b".rdata", b".xdata", b".pdata"):
            raise ValueError(f"Unexpected nonempty COFF section {name!r}.")
        alignment = 1 << max(0, ((flags >> 20) & 15) - 1)
        code.extend(bytes(align(len(code), alignment) - len(code)))
        output_offset = len(code)
        section_offsets[number] = output_offset
        code.extend(obj[offset:offset + size])
        if name == b".pdata":
            pdata_ranges.append((output_offset, size))

    strings = symbol_table + 18 * symbol_count
    symbols = {}
    index = 0
    while index < symbol_count:
        raw, value, number, _, storage, aux = struct.unpack_from(
            "<8sIhHBB", obj, symbol_table + 18 * index
        )
        if raw[:4] == bytes(4):
            start = strings + struct.unpack_from("<I", raw, 4)[0]
            name = obj[start:obj.index(0, start)].decode("ascii")
        else:
            name = raw.rstrip(b"\0").decode("ascii")
        if number in section_offsets:
            symbol_offset = section_offsets[number] + value
            symbols[index] = symbol_offset
            if storage == 2:
                labels[name] = symbol_offset
        elif number == 0:
            symbols[index] = name
        index += 1 + aux
    for number, output_offset in section_offsets.items():
        _, _, _, _, _, reloc_offset, _, reloc_count, _, _ = sections[number - 1]
        for pos, index, kind in struct.iter_unpack(
            "<IIH", obj[reloc_offset:reloc_offset + 10 * reloc_count]
        ):
            relocs.append((output_offset + pos, symbols[index], kind))


def patch_executable(stock, build, code, labels, relocs, pdata_ranges, rules, allow_coop, gis_maps=(),
                     *, chances=None, runtime_state=None, enabled=True):
    pe = struct.unpack_from("<I", stock, 0x3C)[0]
    section_count = struct.unpack_from("<H", stock, pe + 6)[0]
    optional_size = struct.unpack_from("<H", stock, pe + 20)[0]
    optional = pe + 24
    section_table = optional + optional_size
    sections = [
        struct.unpack_from("<8sIIIIIIHHI", stock, section_table + 40 * index)
        for index in range(section_count)
    ]
    section_alignment, file_alignment = struct.unpack_from("<II", stock, optional + 32)
    section_header = section_table + 40 * section_count
    if runtime_state is None and section_header + 80 > min(section[4] for section in sections if section[4]):
        raise ValueError("No room for the Armed Citizens Mod PE section header.")
    state = align(max(section[2] + max(section[1], section[3]) for section in sections), section_alignment)
    if runtime_state is not None:
        if not state <= runtime_state < 0x70000000 or runtime_state % section_alignment:
            raise ValueError("Invalid ASI patch allocation.")
        state = runtime_state
    base = align(state + STATE_SIZE, section_alignment)

    def file_offset(rva):
        for _, _, start, length, offset, *_ in sections:
            if start <= rva < start + length:
                return offset + rva - start
        raise ValueError(f"RVA {rva:#x} is outside the stock file.")

    # Resolve RIP-relative code references and image-relative unwind records.
    code.extend(bytes(align(len(code), 4) - len(code)))
    gis_offset = len(code)
    code.extend(struct.pack("<I", len(gis_maps)))
    for name, count, payload in gis_maps:
        encoded_name = name.encode("ascii")
        if len(encoded_name) >= 80:
            raise ValueError(f"GIS map name is too long: {name}")
        code.extend(struct.pack("<II80s", count, len(payload), encoded_name))
        code.extend(payload)
        code.extend(bytes(align(len(code), 4) - len(code)))
    coop_build = hashlib.sha256(code).digest()[:8]
    if coop_build == build.hello_build:
        coop_build = bytes(8)

    code.extend(bytes(align(len(code), 8) - len(code)))
    config_offset = len(code)
    code.extend(rules)
    symbols = build.symbols | {name: base + offset for name, offset in labels.items()}
    symbols["militia_local_rules"] = base + config_offset
    symbols["militia_coop_enabled"] = base + len(code)
    code.append(allow_coop)
    code.extend(bytes(align(len(code), 4) - len(code)))
    symbols["militia_local_rules_size"] = base + len(code)
    code.extend(struct.pack("<I", len(rules)))
    symbols["gis_map_count"] = base + gis_offset
    symbols["gis_maps"] = base + gis_offset + 4
    symbols["mod_initial_settings"] = base + len(code)
    code.extend(menu_settings_data(rules, allow_coop, chances, enabled))
    code.extend(bytes(align(len(code), 2) - len(code)))
    command = ([str(MOD_DIR / "armed-citizens-launcher.exe")] if getattr(sys, "frozen", False)
               else [sys.executable, str(MOD_DIR / "launch.py")])
    symbols["mod_settings_command"] = base + len(code)
    code.extend((subprocess.list2cmdline(command + ["--save-settings"]) + "\0").encode("utf-16-le"))
    symbols.update({name: state + offset for name, offset in STATE_OFFSETS.items()})
    for offset, target, kind in relocs:
        target_rva = base + target if isinstance(target, int) else symbols[target]
        addend = struct.unpack_from("<i", code, offset)[0]
        if kind == 3:  # IMAGE_REL_AMD64_ADDR32NB
            struct.pack_into("<I", code, offset, target_rva + addend)
        elif 4 <= kind <= 9:  # IMAGE_REL_AMD64_REL32 and its trailing-byte variants
            struct.pack_into("<i", code, offset, target_rva + addend - (base + offset + kind))
        else:
            raise ValueError(f"Unexpected COFF relocation {kind} for {target}.")

    # Describe the wrapper and detours for the Windows unwinder.
    exception_directory = optional + 112 + 3 * 8
    pdata, pdata_size = struct.unpack_from("<II", stock, exception_directory)
    pdata_offset = file_offset(pdata)
    original_table = stock[pdata_offset:pdata_offset + pdata_size]
    original_functions = list(struct.iter_unpack("<III", original_table))
    hook_sites = {name: rva for rva, _, _, name in build.hooks}

    def stock_unwind(hook_name):
        rva = hook_sites[hook_name]
        for record in original_functions:
            if record[0] <= rva < record[1]:
                return bytes.fromhex("21 00 00 00") + struct.pack("<III", *record)
        raise ValueError(f"Missing stock unwind information for {hook_name}.")

    code.extend(bytes(align(len(code), 4) - len(code)))
    battle_unwind = base + len(code)
    if code[labels["battle_start"]:labels["battle_start"] + 4] != bytes.fromhex("48 83 ec 28"):
        raise ValueError("Battle-start prologue no longer matches its unwind information.")
    code.extend(bytes.fromhex("01 04 01 00 04 42 00 00"))
    arming_unwind = base + len(code)
    code.extend(stock_unwind("sample_civilian"))
    tick_unwind = base + len(code)
    code.extend(stock_unwind("add_militia_after_tick"))
    ranking_unwind = base + len(code)
    code.extend(stock_unwind("rank_completed_battle"))
    recount_unwind = base + len(code)
    # The recount detour runs inside the stock frame. Describe its saved
    # XMM6-8, 0xc8-byte allocation and eight saved general registers.
    code.extend(bytes.fromhex(
        "01 00 10 00  00 68 09 00  00 78 0a 00  00 88 0b 00  00 01 19 00 "
        "00 30  00 50  00 70  00 60  00 c0  00 d0  00 e0  00 f0"
    ))
    toolbar_unwind = base + len(code)
    code.extend(stock_unwind("add_building_types_button"))
    battle_toolbar_unwind = base + len(code)
    code.extend(stock_unwind("append_battle_view_button"))
    color_unwind = base + len(code)
    # The native color routine has no .pdata entry. Its frame at the hook
    # holds XMM6–15, eight general registers and a 0x100f8-byte allocation.
    code.extend(bytes.fromhex(
        "01 00 1e 00 "
        "00 68 05 10  00 78 06 10  00 88 07 10  00 98 08 10  00 a8 09 10 "
        "00 b8 0a 10  00 c8 0b 10  00 d8 0c 10  00 e8 0d 10  00 f8 0e 10 "
        "00 01 1f 20  00 30  00 50  00 70  00 60  00 c0  00 d0  00 e0  00 f0"
    ))
    building_color_unwind = base + len(code)
    # The final color lookup saves RSI, RDI, RBP and RBX.
    code.extend(bytes.fromhex("01 00 04 00 00 30 00 50 00 70 00 60"))
    statpanel_unwind = base + len(code)
    code.extend(stock_unwind("statpanel_building_use"))
    key_panel_unwind = base + len(code)
    code.extend(stock_unwind("draw_building_type_key_panel"))
    key_text_unwind = base + len(code)
    code.extend(stock_unwind("draw_building_type_key_text"))
    legend_unwind = base + len(code)
    code.extend(bytes.fromhex("01 08 03 00 08 01 12 00 01 30 00 00"))
    invalidate_unwind = base + len(code)
    code.extend(stock_unwind("invalidate_inferred_building_uses"))
    settings_unwind = {}
    for name in SETTINGS_HOOKS:
        settings_unwind[name] = base + len(code)
        code.extend(stock_unwind(name))

    injected_functions = []
    for offset, size in pdata_ranges:
        injected_functions.extend(struct.iter_unpack("<III", code[offset:offset + size]))
    for name, unwind in (
        ("battle_start", battle_unwind),
        ("sample_civilian", arming_unwind),
        ("militia_source", arming_unwind),
        ("count_living_militia", recount_unwind),
        ("add_militia_after_tick", tick_unwind),
        ("rank_completed_battle", ranking_unwind),
        ("add_building_types_button", toolbar_unwind),
        ("append_battle_view_button", battle_toolbar_unwind),
        ("draw_three_view_buttons", toolbar_unwind),
        ("lower_click_paint_rows", toolbar_unwind),
        ("apply_building_type_colors", color_unwind),
        ("prefer_building_type_color", building_color_unwind),
        ("statpanel_building_use", statpanel_unwind),
        ("draw_building_type_key_panel", key_panel_unwind),
        ("draw_building_type_key_text", key_text_unwind),
        ("draw_building_type_key", legend_unwind),
        ("invalidate_inferred_building_uses", invalidate_unwind),
        *settings_unwind.items(),
    ):
        if name in labels:
            injected_functions.append((symbols[name], symbols[name + "_end"], unwind))
    new_pdata = base + len(code)
    if runtime_state is None:
        code.extend(original_table)
    for function in sorted(injected_functions):
        code.extend(struct.pack("<III", *function))
    new_pdata_size = base + len(code) - new_pdata

    edits = []
    if enabled and allow_coop:
        for rva in build.hello_sites:
            offset = file_offset(rva)
            if stock[offset:offset + 8] != build.hello_build:
                raise ValueError(f"Unexpected stock co-op build hash at RVA {rva:#x}.")
            edits.append((rva, build.hello_build, coop_build))
        for rva, register in build.ready_sites:
            offset = file_offset(rva)
            expected = bytes((0x48, 0xB8 + register)) + build.ready_seed
            if stock[offset:offset + 10] != expected:
                raise ValueError(f"Unexpected stock co-op settings hash at RVA {rva:#x}.")
            displacement = symbols["militia_coop_ready_seed"] - (rva + 7)
            replacement = (
                bytes((0x48, 0x8B, 0x05 + 8 * register)) + struct.pack("<i", displacement) + b"\x90" * 3
            )
            edits.append((rva, expected, replacement))
    for rva, expected_hex, opcode_hex, destination in build.hooks:
        if not enabled and destination not in SETTINGS_HOOKS:
            continue
        expected = bytes.fromhex(expected_hex)
        opcode = bytes.fromhex(opcode_hex)
        offset = file_offset(rva)
        if stock[offset:offset + len(expected)] != expected:
            raise ValueError(f"Unexpected stock instructions at RVA {rva:#x}.")
        branch_end = rva + len(opcode) + 4
        branch = opcode + struct.pack("<i", symbols[destination] - branch_end)
        edits.append((rva, expected, branch.ljust(len(expected), b"\x90")))

    if runtime_state is not None:
        patch = bytearray(struct.pack("<8s6I", b"ZARASI1\0", state, base - state, len(code),
                                      new_pdata - base, len(injected_functions), len(edits)))
        patch.extend(code)
        for rva, expected, replacement in edits:
            patch.extend(struct.pack("<II", rva, len(expected)))
            patch.extend(expected)
            patch.extend(replacement)
        return patch

    result = bytearray(stock)
    for rva, expected, replacement in edits:
        offset = file_offset(rva)
        result[offset:offset + len(expected)] = replacement

    # Append zero-initialized writable state and a read-only executable section.
    data_offset = align(len(result), file_alignment)
    data_size = align(len(code), file_alignment)
    result.extend(bytes(data_offset - len(result)))
    result.extend(code.ljust(data_size, b"\0"))
    struct.pack_into(
        "<8sIIIIIIHHI", result, section_header,
        b".mltdata", STATE_SIZE, state, 0, 0, 0, 0, 0, 0, 0xC0000080,
    )
    struct.pack_into(
        "<8sIIIIIIHHI", result, section_header + 40,
        b".militia", len(code), base, data_size, data_offset, 0, 0, 0, 0, 0x60000020,
    )
    struct.pack_into("<H", result, pe + 6, section_count + 2)
    code_size = struct.unpack_from("<I", stock, optional + 4)[0]
    struct.pack_into("<I", result, optional + 4, code_size + data_size)
    bss_size = struct.unpack_from("<I", stock, optional + 12)[0]
    struct.pack_into("<I", result, optional + 12, bss_size + STATE_SIZE)
    struct.pack_into("<I", result, optional + 56, align(base + len(code), section_alignment))
    struct.pack_into("<II", result, exception_directory, new_pdata, new_pdata_size)
    return result


def main():
    from us_gis import read_catalog

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true", help="Generate the game copy without launching it.")
    parser.add_argument("--save-settings", nargs="+", help=argparse.SUPPRESS)
    parser.add_argument("--build-asi-patch", nargs=2, metavar=("RVA", "OUTPUT"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.save_settings is not None:
        save_menu_settings(MOD_DIR / "settings.ini", args.save_settings)
        return
    stock = STOCK_EXE.read_bytes()
    build = SUPPORTED_BUILDS.get(hashlib.sha256(stock).hexdigest())
    if build is None:
        versions = ", ".join(f"v{item.version}" for item in SUPPORTED_BUILDS.values())
        raise ValueError(f"Unsupported game executable. Supported Windows Steam builds are {versions}.")
    chances, rules, allow_coop, enabled = read_settings(MOD_DIR / "settings.ini")
    if enabled and allow_coop and len(rules) > COOP_RULES_CAPACITY:
        raise ValueError("The co-op map list is too long. Remove some IDs from extra_us_map_ids.")
    gis_maps = read_catalog(GAME_DIR) if enabled else ()
    with tempfile.TemporaryDirectory(prefix="zar-starting-militia-", dir=GAME_DIR) as temp:
        directory = Path(temp)
        code, labels, relocs, pdata_ranges = assemble(directory, build)
        runtime_state = int(args.build_asi_patch[0], 16) if args.build_asi_patch else None
        patched = patch_executable(stock, build, code, labels, relocs, pdata_ranges,
                                   rules, allow_coop, gis_maps, chances=chances, runtime_state=runtime_state,
                                   enabled=enabled)
        if args.build_asi_patch:
            Path(args.build_asi_patch[1]).write_bytes(patched)
            mode = "gameplay and settings" if enabled else "settings only (mod disabled)"
            print(f"Prepared Armed Citizens Mod for v{build.version}: {mode}.", flush=True)
            return
        if getattr(sys, "frozen", False):
            digest = hashlib.sha256(patched).hexdigest()
            executable = GAME_DIR / f"zombies-arent-real-militia-{digest[:16]}.exe"
            if not executable.exists():
                output = directory / executable.name
                output.write_bytes(patched)
                try:
                    # Windows rename fails if another launcher already created the file.
                    output.rename(executable)
                except FileExistsError:
                    pass
            if hashlib.sha256(executable.read_bytes()).hexdigest() != digest:
                raise ValueError(f"The generated file {executable.name} has changed. Remove it and try again.")
        else:
            executable = MODDED_EXE
            output = directory / executable.name
            output.write_bytes(patched)
            os.replace(output, executable)
    print(
        f"Armed Citizens Mod for v{build.version} built with {chances['residential']}% residential, "
        f"{chances['commercial']}% commercial, and {chances['other']}% elsewhere.",
        flush=True,
    )
    if not args.build_only:
        # Use the same Steam app identity as the installed game.
        env = os.environ.copy()
        env["SteamAppId"] = "5185890"
        env["SteamGameId"] = "5185890"
        if getattr(sys, "frozen", False):
            # PyInstaller's bundled DLL search directory belongs to this launcher.
            import ctypes

            ctypes.windll.kernel32.SetDllDirectoryW(None)
            env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
        subprocess.Popen([executable], cwd=GAME_DIR, env=env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, InvalidOperation, configparser.Error, subprocess.CalledProcessError) as error:
        sys.exit(f"Armed Citizens Mod: {error}")
