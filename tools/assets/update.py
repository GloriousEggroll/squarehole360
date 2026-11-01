"""Find the game's title update in folders and archives and unpack it for the game.

The recompiled executable is built from the disc executable with the title
update applied, so the update is required. Its package (one per disc media
ID; only the one for the supported disc is listed in the game definition) is
matched by file name, size and SHA-256, then unpacked to USER/update/. The
game mounts that folder as update:, where it also finds the executable patch
(default.xexp) the recompiler reads.
"""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import zipfile

from .dlc import _tar_member, _zip_member, is_archive
from .stfs import Package
from .xdvdfs import AssetError

MARKER = "package.json"


@dataclass
class Found:
    update: object   # games.TitleUpdate
    source: str
    open: object


def folder(user_root):
    return Path(user_root) / "update"


def find(sources, game):
    """The game's title update package in the given folders and archives, or None."""
    update = game.title_update
    if update is None:
        return None
    for source in sources:
        source = Path(source).expanduser()
        if source.is_file() and source.name.casefold() == update.file.casefold():
            if source.stat().st_size == update.size:
                return Found(update, str(source), lambda path=source: path.open("rb"))
        elif source.is_dir():
            for parent, dirs, names in os.walk(source):
                for name in sorted(names):
                    path = Path(parent) / name
                    found = find([path], game) if (name.casefold() == update.file.casefold() or is_archive(name)) else None
                    if found:
                        return found
        elif source.is_file() and source.name.lower().endswith(".zip"):
            with zipfile.ZipFile(source) as archive:
                for info in archive.infolist():
                    if Path(info.filename).name.casefold() == update.file.casefold() and info.file_size == update.size:
                        return Found(update, f"{source.name}: {info.filename}",
                                     lambda source=source, name=info.filename: _zip_member(source, name))
        elif source.is_file() and is_archive(source):
            with tarfile.open(source, "r:*") as archive:
                for member in archive:
                    if (member.isfile() and Path(member.name).name.casefold() == update.file.casefold()
                            and member.size == update.size):
                        return Found(update, f"{source.name}: {member.name}",
                                     lambda source=source, name=member.name: _tar_member(source, name))
        elif not source.exists():
            raise AssetError(f"Not found: {source}")
    return None


def installed(game, user_root):
    """Whether the title update is unpacked: its marker and executable patch match."""
    update = game.title_update
    if update is None:
        return True
    root = folder(user_root)
    try:
        marker = json.loads((root / MARKER).read_text(encoding="utf-8"))
        patch = (root / "default.xexp").read_bytes()
    except (OSError, ValueError):
        return False
    return marker.get("sha256") == update.sha256 and hashlib.sha256(patch).hexdigest() == update.xexp_sha256


def check(game, user_root):
    if not installed(game, user_root):
        update = game.title_update
        raise AssetError(f"{game.title} {update.name} is not installed. Run the setup wizard to add it.")


def install(found, game, user_root, progress):
    """Verify the package and unpack it to USER/update (replacing an older one)."""
    update = found.update
    progress("update", 0, 1, update.name)
    with found.open() as stream:
        data = stream.read(update.size + 1)
    if len(data) != update.size or hashlib.sha256(data).hexdigest() != update.sha256:
        raise AssetError(f"{update.name} from {found.source} does not match the known package")
    package = Package(data)
    if package.title_id != game.title_id or package.content_type != "000B0000":
        raise AssetError(f"{found.source} is not a {game.title} title update")
    root = folder(user_root)
    staging = root.with_name(root.name + ".new")
    shutil.rmtree(staging, ignore_errors=True)
    digests = package.extract(staging)
    if digests.get("default.xexp") != update.xexp_sha256:
        shutil.rmtree(staging, ignore_errors=True)
        raise AssetError(f"{update.name} does not contain the expected executable patch")
    (staging / MARKER).write_text(json.dumps({"name": update.name, "file": update.file, "sha256": update.sha256},
                                             indent=1) + "\n", encoding="utf-8")
    shutil.rmtree(root, ignore_errors=True)
    os.replace(staging, root)
    progress("complete", 1, 1, f"Installed {update.name}")
    return update.name
