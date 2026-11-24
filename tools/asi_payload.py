"""Turn the mod's COFF objects and supported-build metadata into an ASI link recipe."""

import json

from launch import (
    CHANCE_DEFAULTS, COOP_RULES_CAPACITY, MENU_SETTINGS, SETTINGS_HOOKS,
    STATE_OFFSETS, STATE_SIZE, SUPPORTED_BUILDS, US_MAP_IDS,
    align, append_unwind_info, assemble,
)
from sf_gis import MAP_HASH_VARIANTS, MAP_NAME
from us_gis import SOURCES


DYNAMIC_SYMBOLS = (
    "gis_map_count", "gis_maps", "militia_local_rules", "militia_coop_enabled",
    "militia_local_rules_size", "mod_initial_settings",
)


def bytes_initializer(value):
    return "{" + ",".join(f"0x{byte:02x}" for byte in value) + "}"


def array(name, type_name, rows):
    return f"static const {type_name} {name}[] = {{\n" + ",\n".join(rows) + "\n};\n"


def write_payload(work):
    save_offset = align(STATE_SIZE, 8)
    output = [
        f"enum {{ SAVE_OFFSET = {save_offset}, CODE_OFFSET = {align(save_offset + 8, 4096)}, "
        f"READY_SEED_OFFSET = {STATE_OFFSETS['militia_coop_ready_seed']} }};",
        f"_Static_assert(DYNAMIC_COUNT == {len(DYNAMIC_SYMBOLS)}, \"ASI dynamic symbols\");",
    ]
    rows = []
    for name, cache in [(MAP_NAME, "san_francisco_gis.bin"), *(
        (entry[0], f"gis/{entry[0][:-4]}.bin") for entry in SOURCES.values()
    )]:
        alternate = MAP_HASH_VARIANTS.get(name, ("00" * 32, "00" * 32))
        hashes = ", ".join(bytes_initializer(bytes.fromhex(value)) for value in alternate)
        rows.append(f'{{"{name}", L"{name}", L"{cache}", {hashes}}}')
    output.append(array("gis_sources", "GisSource", rows))

    build_rows = []
    for index, (digest, build) in enumerate(SUPPORTED_BUILDS.items()):
        code, labels, relocations, pdata_ranges = assemble(work, build, native_asi=True)
        hook_rvas = {name: rva for rva, _, _, name in build.hooks}
        chains = []

        def stock_chain(name):
            chains.append((len(code) + 4, hook_rvas[name]))
            return bytes.fromhex("21 00 00 00") + bytes(12)

        frames = append_unwind_info(code, labels, 0, stock_chain)
        prefix = f"build_{index}"
        output.append(array(prefix + "_code", "BYTE", (
            ",".join(f"0x{byte:02x}" for byte in code[pos:pos + 24])
            for pos in range(0, len(code), 24)
        )))
        targets = {name: ("TARGET_IMAGE", rva) for name, rva in build.symbols.items()}
        targets.update({name: ("TARGET_CODE", offset) for name, offset in labels.items()})
        targets.update({name: ("TARGET_STATE", offset) for name, offset in STATE_OFFSETS.items()})
        targets.update({name: ("TARGET_DYNAMIC", i) for i, name in enumerate(DYNAMIC_SYMBOLS)})
        targets["native_save_settings"] = ("TARGET_STATE", save_offset)
        rows = []
        for offset, symbol, kind in relocations:
            region, target = ("TARGET_CODE", symbol) if isinstance(symbol, int) else targets[symbol]
            if not 3 <= kind <= 9:
                raise ValueError(f"Unsupported ASI relocation {kind}.")
            rows.append(f"{{{offset},{target},{region},{kind}}}")
        output.append(array(prefix + "_relocs", "Relocation", rows))
        output.append(array(prefix + "_chains", "StockChain", (f"{{{offset},{rva}}}" for offset, rva in chains)))
        output.append(array(prefix + "_frames", "RUNTIME_FUNCTION", (
            f"{{{start},{end},{{{unwind}}}}}" for start, end, unwind in frames
        )))
        output.append(array(prefix + "_pdata", "PdataRange", (
            f"{{{offset},{size // 12}}}" for offset, size in pdata_ranges
        )))
        hooks = []
        for rva, expected_hex, opcode_hex, name in build.hooks:
            expected = bytes.fromhex(expected_hex)
            opcode = list(bytes.fromhex(opcode_hex))
            if len(expected) > 32 or len(opcode) > 2:
                raise ValueError(f"Unexpected hook width for {name}.")
            op = "{" + ",".join(str(byte) for byte in opcode) + "}"
            hooks.append(f"{{{rva},{labels[name]},{len(expected)},{len(opcode)},"
                         f"{int(name in SETTINGS_HOOKS)},{op},{bytes_initializer(expected)}}}")
        output.append(array(prefix + "_hooks", "Hook", hooks))
        output.append(array(prefix + "_hello", "DWORD", map(str, build.hello_sites)))
        output.append(array(prefix + "_ready", "ReadySite", (f"{{{rva},{reg}}}" for rva, reg in build.ready_sites)))
        members = [f'"{build.version}"', bytes_initializer(bytes.fromhex(digest)),
                   bytes_initializer(build.hello_build), bytes_initializer(build.ready_seed)]
        for suffix in ("code", "relocs", "chains", "frames", "pdata", "hooks", "hello", "ready"):
            name = f"{prefix}_{suffix}"
            members.extend((name, f"_countof({name})"))
        build_rows.append("{" + ",".join(members) + "}")
    output.append(array("native_builds", "NativeBuild", build_rows))
    (work / "asi_payload.h").write_text("\n".join(output), encoding="ascii")
    keys = (*MENU_SETTINGS, "chance_percent")
    (work / "asi_config_data.h").write_text(
        array("menu_keys", "char *const", map(json.dumps, keys)) +
        array("chance_defaults", "char *const", (json.dumps(CHANCE_DEFAULTS[key]) for key in
              ("residential", "commercial", "other"))) +
        array("us_maps", "char *const", (json.dumps(name.decode("ascii")) for name in US_MAP_IDS)),
        encoding="ascii",
    )
    (work / "asi_config_limits.h").write_text(
        f"enum {{ MENU_FIELDS = {len(MENU_SETTINGS)}, COOP_RULES_LIMIT = {COOP_RULES_CAPACITY} }};\n",
        encoding="ascii",
    )
