# Zombies Aren't Real Mod Loader

**Tested game version:** Steam v0.44.0 on Windows x64.

1. In Steam, open **Manage → Browse local files** for *Zombies Aren't Real*.
2. Copy `winhttp.dll` next to `zombies-arent-real.exe`. If you already have a compatible x64 ASI loader that calls `InitializeASI`, keep it.
3. Install compatible x64 ASI plugins in the game's `scripts` folder. Follow each plugin's instructions for its additional files.
4. Start the game with **Play** in Steam.

Armed Citizens is available as a separate plugin. Each plugin specifies its own supported game builds. [Armed Citizens release](https://github.com/Toys0125/Zombies_Arent_Real_Mod_Loader/releases/tag/armed-citizens-v1.0.0)

To remove the bundled loader, close the game and remove the `winhttp.dll` you installed in step 2. Removing the loader disables ASI loading through that DLL.

This package uses the Windows x64 build of Ultimate ASI Loader v10.0.5. Its terms are included in `licenses/Ultimate-ASI-Loader.txt`. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/tag/v10.0.5)
