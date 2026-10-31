"""Setup step: compile every pixel shader in the user's game files ahead of time.

Runs gow3-shader-precompile, which reads the imported game (and unpacked DLC),
extracts its pixel shaders and compiles them into the game's own shader cache
under <user>/cache. The first visit to an area then finds its pixel shaders
ready instead of compiling them mid-game. Nothing is written outside the user
folder and a rerun only compiles what is missing.
"""
import os
from pathlib import Path
import subprocess

from .xdvdfs import AssetError

ROOT = Path(__file__).resolve().parents[2]
TOOL = "gow3-shader-precompile"


def find_tool():
    name = TOOL + (".exe" if os.name == "nt" else "")
    for folder in ("build-windows/bin", "build/bin", "build-sniper/bin"):
        path = ROOT / folder / name
        if path.is_file():
            return path
    return None


def inputs(game_root, user_root):
    folders = [Path(game_root) / "GearGame" / "CookedXbox360"]
    # The game unpacks installed DLC on its next start; include it once it has.
    content = Path(user_root) / "0000000000000000"
    if content.is_dir():
        folders.append(content)
    return [folder for folder in folders if folder.is_dir()]


def precompile(game_root, user_root, progress):
    """Compiles missing shaders; returns the tool's summary line."""
    tool = find_tool()
    if tool is None:
        raise AssetError(f"{TOOL} is missing from this installation")
    folders = inputs(game_root, user_root)
    if not folders:
        raise AssetError("The imported game files were not found")
    command = [str(tool), str(Path(user_root) / "cache"), *map(str, folders)]
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    messages = {"scan": "Finding shaders in the game files", "compile": "Compiling shaders"}
    summary = ""
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                          creationflags=flags) as process:
        for line in process.stdout:
            parts = line.split()
            if len(parts) == 4 and parts[0] == "progress":
                done, total = int(parts[2]), int(parts[3])
                progress("shaders", done, total, f"{messages.get(parts[1], parts[1])} ({done:,} of {total:,})")
            elif parts and parts[0] == "done":
                summary = line.strip()
    if process.returncode:
        raise AssetError(f"Shader precompilation failed (exit code {process.returncode})")
    return summary
