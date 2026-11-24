# Zombies Aren't Real Mod Loader

This project packages the Windows x64 setup for loading ASI plugins in *Zombies Aren't Real*. It uses Ultimate ASI Loader as `winhttp.dll` in the game folder. Put compatible x64 `.asi` files in the game's shared `scripts` folder and start the game with **Play** in Steam. Each plugin can have its own game version requirements and configuration.

The loader is available on its own and in an all-in-one Armed Citizens ZIP. It was tested with **Steam v0.44.0 on Windows x64**. [Mod Loader release](https://github.com/Toys0125/Zombies_Arent_Real_Mod_Loader/releases/tag/loader-v1.0.0) · [Armed Citizens release](https://github.com/Toys0125/Zombies_Arent_Real_Mod_Loader/releases/tag/armed-citizens-v1.0.0)

## Armed Citizens Mod

Armed Citizens is available as an ASI plugin. Civilians can start as militia and still count as civilians. Starting militia is enabled on supported U.S. maps by default. The **Building Types** view and the top-right **Use** row show building classifications from map tags, storefronts, local GIS data, and footprint inference.

### Install and play

The ASI release was tested on **Steam v0.44.0, Windows x64**. The plugin also includes compatibility code for Steam v0.43.1, which was build-checked for this release.

1. Choose **Armed-Citizens-Mod-v0.44.0-asi-with-loader.zip** for a fresh install, or **Armed-Citizens-Mod-v0.44.0-asi.zip** if you already have a compatible x64 ASI loader that calls `InitializeASI`.
2. In Steam, right-click *Zombies Aren't Real* in your Library and choose **Manage → Browse local files**. This opens the folder containing `zombies-arent-real.exe`.
3. Extract `scripts` and `starting_militia` into the game folder. The all-in-one ZIP also includes `winhttp.dll`, which goes next to the game's executable. Keep a copy of your existing `starting_militia/settings.ini` if you customized it.
4. Launch the game from Steam. To confirm the mod loaded, open **Settings → Mods** or check `starting_militia/asi.log`.

These files should be in your game folder before the first launch.

```text
Zombies-Arent-Real/
├── zombies-arent-real.exe
├── winhttp.dll
├── scripts/
│   └── Armed Citizens Mod.asi
└── starting_militia/
    ├── armed-citizens-launcher.exe
    ├── settings.ini
    ├── san_francisco_gis.bin
    └── gis/
```

Ultimate ASI Loader uses the game's WinHTTP import. The plugin runs its bundled helper to validate the installed build and prepare the mod in memory. Steam's executable stays on disk as shipped, and the ASI workflow creates no alternate game executable. The Armed Citizens ZIP contains the plugin, helper, settings, GIS caches, and documentation. The game executable, maps, and assets come from Steam.

Choose **Building Types** in the View controls and hover over a building to see its **Use**. During planning, the legend estimates eligible civilians in each category and the total expected starting militia. After **Ready**, it shows the actual eligible counts and militia spawned. During battle, use **Building Types** beside **Restart** to toggle the overlay.

### Settings and updates

- Open **Settings → Mods**, just below **Game Files**. The **Armed Citizens Mod** section contains the three spawn rates, **Allow all maps**, **Enable co-op**, and **Extra U.S. map IDs**. Click a value to edit it or toggle it. Choose **Save Changes**, then relaunch the game.
- Turn **Enable mod** off to restore stock gameplay, including the stock co-op rules. Save and restart to apply the change. The same page remains available to turn the mod back on.
- The defaults are **4%** residential, **0.025%** commercial, and **0.25%** elsewhere. Extra map IDs are comma-separated file names without `.map`. Daily challenges skip the roll.
- You can also edit the installed `starting_militia/settings.ini`. The launcher reads the same file when you start the mod.
- Eight packaged GIS lookups work offline. The mod checks each map against its cache and uses its regular inference rules when a cache is unavailable or the map has changed.
- To update, copy the new ZIP's files into the game folder. Save a copy of your installed `settings.ini` first if you customized it. The new build takes effect on the next launch.
- If the mod skips loading, check `starting_militia/asi.log` for an unsupported game build, missing helper, or incompatible in-memory edit.
- To uninstall the ASI, close the game and remove `scripts/Armed Citizens Mod.asi` and `starting_militia`. Remove `winhttp.dll` if you installed it for Armed Citizens and no other plugins use it.

#### Standalone launcher

The optional `-windows.zip` build uses the original standalone setup. Copy **Play Armed Citizens Mod.cmd** and **starting_militia** into the game folder, then double-click the CMD to play. It creates a separate `zombies-arent-real-militia-<id>.exe`. To uninstall this setup, remove the CMD, the **starting_militia** folder, and the generated militia executables.

### Co-op

1. Both players use the same Steam game build, Armed Citizens Mod release, and map files.
2. Both players turn on **Enable co-op** in **Settings → Mods**, save, and relaunch the mod.
3. Host, join, and ready up as usual. The game shares the host's spawn chances and map settings automatically. The guest's own settings still apply in solo play.

If the game reports a version mismatch, check that both players enabled co-op and have matching game builds, mod releases, and map files for the bundled GIS caches.

### Build from source

The source launcher runs with [Python 3.9 or newer](https://www.python.org/downloads/windows/) and [LLVM clang](https://github.com/llvm/llvm-project/releases). Put `python` on PATH and put `clang.exe` on PATH or in `C:\Program Files\LLVM\bin`. Copy the repo's launcher and `starting_militia` folder into the game folder. Use the same double-click launcher or run `python .\starting_militia\launch.py --build-only`.

Run `python tools/package_release.py --loader` to download and package the pinned, checksum-verified x64 build of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). To package Armed Citizens, install [PyInstaller](https://pyinstaller.org/), LLVM, and Visual Studio Build Tools with **Desktop development with C++**, then run `python tools/package_release.py --asi`. After building both ZIPs, run `python tools/package_release.py --bundle` for the all-in-one ZIP. Omit the option to package the standalone launcher. ZIPs are written to `dist/`.

### GIS sources

The cached lookups are derived from [San Francisco DataSF](https://data.sf.gov/d/c5ge-t6pj), [Boston Planning](https://gis.bostonplans.org/hosting/rest/services/FY26_Parcels_Detailed/MapServer/0), [Chicago CMAP](https://services5.arcgis.com/LcMXE3TFhi1BSaCY/arcgis/rest/services/LUI20_geodatabase_v1_CMAP/FeatureServer/0), [Detroit Assessing](https://services2.arcgis.com/qvkbeam7Wirps6zC/arcgis/rest/services/Parcels_Current/FeatureServer/0), [Miami-Dade Existing Land Use](https://gisweb.miamidade.gov/ArcGIS/rest/services/MD_Communications/MapServer/2), [New York City MapPLUTO](https://services5.arcgis.com/GfwWNkhOj9bNBqoJ/arcgis/rest/services/MAPPLUTO/FeatureServer/0), [Seattle Existing Land Use](https://services.arcgis.com/ZOyb2t4B0UYuYNYH/arcgis/rest/services/Existing_Land_Use_(20-category)/FeatureServer/0), and [Washington, D.C. Existing Land Use](https://maps2.dcgis.dc.gov/DCGIS/rest/services/DCGIS_DATA/Planning_Landuse_and_Zoning_WebMercator/MapServer/4).

The San Francisco cache also covers Presidio housing areas such as Baker Beach and East and West Washington. Those areas supplement gaps in the city parcels and were checked against current leasing listings. [Presidio Trust housing map, printed page 9](https://wp.presidio.gov/wp-content/uploads/2023/07/PLN-301-PTMP01-ExecSum-1.pdf#page=11) · [Presidio Residences listings](https://www.presidio-residences.com/searchlisting)

## License

The mod code is licensed under the Apache License, Version 2.0. GIS source data and bundled dependencies are subject to their respective terms. [LICENSE](LICENSE)
