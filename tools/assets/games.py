"""Per-game asset definitions: the files a game's disc must provide and its known DLC.

Each supported game has tools/assets/games/<id>.json, created from a verified
import of the user's own disc and DLC. It lists names, sizes and SHA-256
digests only, never game content. The setup wizard and importer read it to
recognize a game's files in ISOs, folders and archives.
"""
from dataclasses import dataclass
import json
from pathlib import Path

from .xdvdfs import AssetError, safe_path


GAMES = Path(__file__).resolve().parent / "games"
DEFAULT_GAME = "gow3"


def _digest(value):
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise AssetError("Invalid digest in game definition")
    return value


@dataclass(frozen=True)
class Dlc:
    name: str
    file: str
    size: int
    sha256: str
    content_type: str


@dataclass(frozen=True)
class Game:
    id: str
    title: str
    title_id: str
    executable: str
    xex_sha256: str
    global_shaders: str
    directories: tuple
    files: tuple
    dlc: tuple

    def base_file(self, path):
        """The listed base-game file for a relative path, matched case-insensitively."""
        return self._by_folded().get(path.casefold())

    def _by_folded(self):
        cache = self.__dict__.get("_folded")
        if cache is None:
            cache = {entry["path"].casefold(): entry for entry in self.files}
            object.__setattr__(self, "_folded", cache)
        return cache

    def dlc_package(self, name):
        """The listed DLC package for a file name, matched case-insensitively."""
        return next((package for package in self.dlc if package.file.casefold() == name.casefold()), None)


def load_game(game_id=DEFAULT_GAME, path=None):
    path = Path(path) if path else GAMES / f"{game_id}.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if value["format"] != "squarehole360.game" or value["version"] != 1:
            raise AssetError(f"Unsupported game definition: {path}")
        files, folded = [], set()
        for entry in value["base"]["files"]:
            safe_path(entry["path"])
            if entry["path"].casefold() in folded or not isinstance(entry["size"], int) or entry["size"] < 0:
                raise AssetError("Invalid or duplicate base file in game definition")
            folded.add(entry["path"].casefold())
            files.append(dict(path=entry["path"], size=entry["size"], sha256=_digest(entry["sha256"])))
        for directory in value["base"]["directories"]:
            safe_path(directory)
        dlc = []
        for entry in value["dlc"]:
            if "/" in entry["file"] or "\\" in entry["file"] or not entry["file"] or entry["size"] <= 0:
                raise AssetError("Invalid DLC package in game definition")
            dlc.append(Dlc(entry["name"], entry["file"], entry["size"], _digest(entry["sha256"]), entry["content_type"]))
        if not any(entry["path"] == value["executable"] for entry in files):
            raise AssetError("The game definition does not list its executable")
        return Game(value["id"], value["title"], value["title_id"], value["executable"], _digest(value["xex_sha256"]),
                    value["global_shaders"], tuple(value["base"]["directories"]), tuple(files), tuple(dlc))
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as exc:
        raise AssetError(f"Cannot read game definition {path}: {exc}") from exc
