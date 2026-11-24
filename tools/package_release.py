"""Build the Windows releases of the shared ASI loader and Armed Citizens Mod."""

import argparse
from contextlib import contextmanager
from hashlib import sha256
from importlib.metadata import distribution
from io import BytesIO
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.request import urlopen
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
MOD_DIR = ROOT / "starting_militia"
sys.path.insert(0, str(MOD_DIR))
from launch import SUPPORTED_BUILDS, compile_objects

LOADER_RELEASE = "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v10.0.5"
LOADER_ASSET = "Ultimate-ASI-Loader-NoPDB_x64.zip"
LOADER_SHA256 = "0f6da6fdbe462986b788518d21ef43c79012059ef726266a1bc0f94168dd6c2e"
LOADER_LICENSE = "https://raw.githubusercontent.com/ThirteenAG/Ultimate-ASI-Loader/v10.0.5/license"
LOADER_PACKAGE_VERSION = "1.0.0"
GAME_VERSION = "0.44.0"
LOADER_ARCHIVE = f"Zombies-Arent-Real-Mod-Loader-v{LOADER_PACKAGE_VERSION}-windows-x64.zip"
PLUGIN_ARCHIVE = f"Armed-Citizens-Mod-v{GAME_VERSION}-asi.zip"
BUNDLE_ARCHIVE = f"Armed-Citizens-Mod-v{GAME_VERSION}-asi-with-loader.zip"


def msvc_environment():
    if os.environ.get("VSCMD_ARG_TGT_ARCH") == "x64":
        return os.environ.copy()
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output(
        [vswhere, "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"], text=True,
    ).strip()
    if not install:
        raise ValueError("Install Visual Studio Build Tools with the Desktop development with C++ workload.")
    setup = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    env = os.environ.copy()
    env["PATH"] = f"{vswhere.parent}{os.pathsep}{env['PATH']}"
    variables = subprocess.check_output(
        f'"{setup}" >nul && set', env=env, shell=True, text=True, errors="replace",
    )
    return dict(line.split("=", 1) for line in variables.splitlines() if "=" in line and not line.startswith("="))


def build_asi(work):
    clang = shutil.which("clang-cl") or Path(os.environ["ProgramFiles"]) / "LLVM/bin/clang-cl.exe"
    output = work / "Armed Citizens Mod.asi"
    subprocess.run(
        [clang, "--target=x86_64-pc-windows-msvc", "/nologo", "/O2", "/W4", "/WX", "/MT", "/LD",
         MOD_DIR / "asi_loader.c", "/link", f"/OUT:{output}"],
        cwd=work, env=msvc_environment(), check=True,
    )
    return output


def loader_files():
    with urlopen(f"{LOADER_RELEASE}/{LOADER_ASSET}", timeout=30) as response:
        download = response.read()
    if sha256(download).hexdigest() != LOADER_SHA256:
        raise ValueError("The Ultimate ASI Loader download did not match its release checksum.")
    with ZipFile(BytesIO(download)) as archive:
        dll = archive.read("dinput8.dll")
    with urlopen(LOADER_LICENSE, timeout=30) as response:
        return dll, response.read()


@contextmanager
def release_archive(name):
    output = ROOT / "dist" / name
    output.parent.mkdir(exist_ok=True)
    # Stage beside the final archive so the rename works across drive letters.
    with tempfile.TemporaryDirectory(prefix="package-", dir=output.parent) as staging:
        archive = Path(staging) / output.name
        with ZipFile(archive, "w", compression=ZIP_DEFLATED, compresslevel=9) as package:
            yield package
        os.replace(archive, output)
    print(f"Packaged {output} ({output.stat().st_size:,} bytes).")


def package_loader():
    loader, license_text = loader_files()
    with release_archive(LOADER_ARCHIVE) as package:
        package.write(ROOT / "tools" / "loader-readme.md", "README.md")
        package.writestr("winhttp.dll", loader)
        package.writestr("licenses/Ultimate-ASI-Loader.txt", license_text)


def package_bundle():
    dist = ROOT / "dist"
    with ZipFile(dist / PLUGIN_ARCHIVE) as plugin, ZipFile(dist / LOADER_ARCHIVE) as loader:
        if {"winhttp.dll", "licenses/Ultimate-ASI-Loader.txt"} & set(plugin.namelist()):
            raise ValueError("Rebuild the plugin-only archive with --asi before creating the bundle.")
        # Keep the shipped binaries identical in both install options.
        with release_archive(BUNDLE_ARCHIVE) as package:
            package.write(ROOT / "README.md", "README.md")
            for name in plugin.namelist():
                if name != "README.md":
                    package.writestr(name, plugin.read(name))
            for name in ("winhttp.dll", "licenses/Ultimate-ASI-Loader.txt"):
                package.writestr(name, loader.read(name))


def package_armed_citizens(asi):
    if sys.platform != "win32" or sys.maxsize <= 2**32:
        raise SystemExit("Build Armed Citizens with 64-bit Python on Windows.")
    archive = PLUGIN_ARCHIVE if asi else f"Armed-Citizens-Mod-v{GAME_VERSION}-windows.zip"
    with tempfile.TemporaryDirectory(prefix="armed-citizens-release-") as temp:
        work = Path(temp)
        plugin = build_asi(work) if asi else None
        objects = sorted({obj for build in SUPPORTED_BUILDS.values() for obj in compile_objects(work, build)})
        command = [
            sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--noupx",
            "--name", "armed-citizens-launcher", "--distpath", str(work),
            "--workpath", str(work / "build"), "--specpath", str(work),
        ]
        for obj in objects:
            command.extend(("--add-data", f"{obj};."))
        subprocess.run([*command, str(MOD_DIR / "launch.py")], cwd=work, check=True)

        with release_archive(archive) as package:
            for name in ("README.md", "LICENSE"):
                package.write(ROOT / name, name)
            if plugin:
                package.write(plugin, "scripts/" + plugin.name)
            else:
                package.write(ROOT / "Play Armed Citizens Mod.cmd", "Play Armed Citizens Mod.cmd")
            package.write(work / "armed-citizens-launcher.exe", "starting_militia/armed-citizens-launcher.exe")
            for path in (MOD_DIR / "settings.ini", MOD_DIR / "san_francisco_gis.bin", *sorted((MOD_DIR / "gis").glob("*.bin"))):
                package.write(path, path.relative_to(ROOT).as_posix())
            python_license = Path(sys.base_prefix) / "LICENSE.txt"
            if python_license.is_file():
                package.write(python_license, "licenses/Python.txt")
            pyinstaller = distribution("pyinstaller")
            for license_file in pyinstaller.files or ():
                if license_file.name.upper().startswith(("COPYING", "LICENSE")) and license_file.parts[0].endswith(".dist-info"):
                    package.write(pyinstaller.locate_file(license_file), "licenses/" + license_file.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    releases = parser.add_mutually_exclusive_group()
    releases.add_argument("--loader", action="store_true", help="Package Ultimate ASI Loader for the game.")
    releases.add_argument("--asi", action="store_true", help="Package the Armed Citizens ASI plugin.")
    releases.add_argument("--bundle", action="store_true", help="Combine the built loader and plugin ZIPs.")
    args = parser.parse_args()
    if args.loader:
        package_loader()
    elif args.bundle:
        package_bundle()
    else:
        package_armed_citizens(args.asi)


if __name__ == "__main__":
    main()
