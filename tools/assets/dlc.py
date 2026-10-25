"""Find a game's DLC packages in folders and archives and install them for the game.

Packages are matched by the file names in the game definition and checked
against its sizes and SHA-256 digests. Verified packages are written to
USER/dlc/, from where the game unpacks each one once at its next start.
"""
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import tarfile
import tempfile
import zipfile

from .xdvdfs import AssetError

ARCHIVE_SUFFIXES = (".zip", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2")


@dataclass
class Found:
    package: object  # games.Dlc
    source: str      # where it was found, for display
    open: object     # callable returning a binary stream


def is_archive(path):
    name = Path(path).name.lower()
    return any(name.endswith(suffix) for suffix in ARCHIVE_SUFFIXES)


def find(sources, game):
    """Known DLC packages in the given folders and archives, one per package."""
    found = {}
    for source in sources:
        source = Path(source).expanduser()
        if source.is_dir():
            for parent, dirs, names in os.walk(source):
                for name in names:
                    package = game.dlc_package(name)
                    path = Path(parent) / name
                    if package and package.file not in found and path.is_file() and path.stat().st_size == package.size:
                        found[package.file] = Found(package, str(path), lambda path=path: path.open("rb"))
                    elif is_archive(name) and path.is_file():
                        for item in find([path], game):
                            found.setdefault(item.package.file, item)
        elif source.is_file() and source.name.lower().endswith(".zip"):
            with zipfile.ZipFile(source) as archive:
                for info in archive.infolist():
                    package = game.dlc_package(Path(info.filename).name)
                    if package and package.file not in found and info.file_size == package.size:
                        found[package.file] = Found(package, f"{source.name}: {info.filename}",
                                                    lambda source=source, name=info.filename: _zip_member(source, name))
        elif source.is_file() and is_archive(source):
            with tarfile.open(source, "r:*") as archive:
                for member in archive:
                    package = game.dlc_package(Path(member.name).name) if member.isfile() else None
                    if package and package.file not in found and member.size == package.size:
                        found[package.file] = Found(package, f"{source.name}: {member.name}",
                                                    lambda source=source, name=member.name: _tar_member(source, name))
        elif not source.exists():
            raise AssetError(f"Not found: {source}")
    return sorted(found.values(), key=lambda item: item.package.name)


class _Member:
    """A stream for one archive member that also closes its archive."""
    def __init__(self, archive, stream):
        self.archive, self.stream = archive, stream

    def read(self, size=-1):
        return self.stream.read(size)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stream.close()
        self.archive.close()


def _zip_member(path, name):
    archive = zipfile.ZipFile(path)
    return _Member(archive, archive.open(name))


def _tar_member(path, name):
    archive = tarfile.open(path, "r:*")
    return _Member(archive, archive.extractfile(name))


def installed(package, game, user_root):
    """Whether the package is already waiting in USER/dlc or installed by the game."""
    user_root = Path(user_root)
    waiting = user_root / "dlc" / package.file
    unpacked = user_root / "0000000000000000" / game.title_id / "Headers" / package.content_type / (package.file + ".header")
    return unpacked.is_file() or (waiting.is_file() and waiting.stat().st_size == package.size)


def install(found, game, user_root, progress):
    """Copy verified packages to USER/dlc; returns the names installed."""
    destination = Path(user_root) / "dlc"
    destination.mkdir(parents=True, exist_ok=True)
    pending = [item for item in found if not installed(item.package, game, user_root)]
    total, done, names = sum(item.package.size for item in pending), 0, []
    for item in pending:
        package = item.package
        digest = hashlib.sha256()
        with tempfile.NamedTemporaryFile(dir=destination, prefix=".dlc-", delete=False) as out:
            try:
                with item.open() as stream:
                    while chunk := stream.read(4 * 1024 * 1024):
                        out.write(chunk)
                        digest.update(chunk)
                        done += len(chunk)
                        progress("dlc", done, total, package.name)
                out.close()
                if digest.hexdigest() != package.sha256 or os.path.getsize(out.name) != package.size:
                    raise AssetError(f"{package.name} from {item.source} does not match the known package")
                os.replace(out.name, destination / package.file)
            except BaseException:
                out.close()
                Path(out.name).unlink(missing_ok=True)
                raise
        names.append(package.name)
    progress("complete", 1, 1, f"Installed {len(names)} DLC package(s); the game unpacks them at its next start"
             if names else "All found DLC is already installed")
    return names
