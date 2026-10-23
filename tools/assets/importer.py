"""User-supplied asset installation and preparation; no bundled game content."""
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from .xdvdfs import AssetError, DiscImage, safe_path


ROOT = Path(__file__).resolve().parents[2]
SUPPORTED_XEX = "4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790"
TITLE_ID = "4D5308AB"
META = ".gow3-assets"
MANIFEST = "manifest.json"
RECIPE_VERSION = 4
GLOBAL_SHADERS = "GearGame/CookedXbox360/GlobalShaderCache-Xbox360.bin"


class Progress:
    def __init__(self, json_output=False):
        self.json_output = json_output
        self.last = 0
        self.phase = None

    def __call__(self, phase, completed, total, message):
        now = time.monotonic()
        if phase == self.phase and completed != total and now - self.last < 0.25:
            return
        self.last, self.phase = now, phase
        percent = int(100 * completed / total) if total else 0
        event = dict(phase=phase, completed=completed, total=total,
                     percent=percent, message=message)
        print(json.dumps(event) if self.json_output else f"{phase}: {percent:3d}%  {message}", flush=True)


def digest_file(path, progress=None):
    digest = hashlib.sha256()
    size, done = path.stat().st_size, 0
    with path.open("rb") as stream:
        while chunk := stream.read(4 * 1024 * 1024):
            digest.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, size)
    return digest.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def is_link(path):
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def asset_path(root, relative):
    path = root
    for part in safe_path(relative).parts:
        path = path / part
        if is_link(path):
            raise AssetError(f"Asset paths must not contain symbolic links or junctions: {path}")
    return path


def output_root(path):
    path = Path(path).expanduser().absolute()
    if is_link(path):
        raise AssetError("The asset destination must not be a symbolic link or junction")
    return path.resolve()


@contextmanager
def destination_lock(root):
    root.parent.mkdir(parents=True, exist_ok=True)
    lock = root.parent / ("." + root.name + ".import.lock")
    try:
        stream = lock.open("x", encoding="utf-8")
    except FileExistsError as exc:
        raise AssetError(f"An import lock already exists: {lock}. If a previous import was interrupted, "
                         "make sure no importer is running before removing that lock.") from exc
    try:
        with stream:
            stream.write(f"pid={os.getpid()}\n")
        yield
    finally:
        lock.unlink()


def kind(path):
    suffix = Path(path).suffix.lower()
    return {".xxx": "cooked-package", ".tfc": "texture-cache", ".bik": "movie",
            ".xex": "executable"}.get(suffix, "game-data")


def identify(disc):
    files = {entry.path.casefold(): entry for entry in disc.entries if not entry.directory}
    xex = files.get("default.xex")
    if not xex:
        raise AssetError("This ISO does not contain default.xex")
    digest = hashlib.sha256()
    for chunk in disc.chunks(xex.offset, xex.size):
        digest.update(chunk)
    if digest.hexdigest() != SUPPORTED_XEX:
        raise AssetError("Unsupported executable. This build requires Gears of War 3 title "
                         f"{TITLE_ID}, default.xex SHA-256 {SUPPORTED_XEX}; "
                         f"the ISO contains {digest.hexdigest()}.")
    if GLOBAL_SHADERS.casefold() not in files:
        raise AssetError("The ISO is missing Gears of War 3's global shader cache")
    if any(entry.path.split("/")[0].casefold() == META for entry in disc.entries):
        raise AssetError("The ISO uses the reserved importer metadata directory")


def check_tree(root, files, directories, metadata_allowed):
    if not root.is_dir():
        raise AssetError(f"Asset directory does not exist: {root}")
    expected = {entry["path"] for entry in files}
    expected_dirs = set(directories)
    found, found_dirs = set(), set()
    for parent, dirs, names in os.walk(root, followlinks=False):
        parent = Path(parent)
        for name in list(dirs):
            path = parent / name
            if is_link(path):
                raise AssetError(f"Linked asset directory: {path}")
            relative = path.relative_to(root).as_posix()
            if relative == META and metadata_allowed:
                dirs.remove(name)
            else:
                found_dirs.add(relative)
        for name in names:
            path = parent / name
            if is_link(path) or not path.is_file():
                raise AssetError(f"Not a regular asset file: {path}")
            found.add(path.relative_to(root).as_posix())
    if found != expected or found_dirs != expected_dirs:
        missing = sorted((expected - found) | (expected_dirs - found_dirs))
        extra = sorted((found - expected) | (found_dirs - expected_dirs))
        raise AssetError(f"Asset tree differs from the import: missing={missing[:3]}, extra={extra[:3]}")


def check_files(root, entries, progress, phase):
    total, done = sum(entry["size"] for entry in entries), 0
    for entry in entries:
        path = asset_path(root, entry["path"])
        if not path.is_file() or path.stat().st_size != entry["size"]:
            raise AssetError(f"Missing asset or size mismatch: {path}")
        digest = digest_file(path, lambda n, _: progress(phase, done + n, total, entry["path"]))
        if digest != entry["sha256"]:
            raise AssetError(f"Asset checksum mismatch: {path}")
        done += entry["size"]
    progress(phase, total, total, f"Verified {len(entries)} files")


def read_manifest(root):
    path = asset_path(root, f"{META}/{MANIFEST}")
    if not path.is_file():
        raise AssetError(f"No import manifest in {root}. Import an ISO first, or use --adopt-existing.")
    if path.stat().st_size > 32 * 1024 * 1024:
        raise AssetError("Asset manifest exceeds the supported size")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        if value["format"] != "gow3.asset-import" or value["version"] != 1:
            raise AssetError("Unsupported asset manifest version")
        if value["xex_sha256"] != SUPPORTED_XEX or value["title_id"] != TITLE_ID:
            raise AssetError("Asset manifest belongs to a different game build")
        for collection in (value["files"], value["prepared"]["files"]):
            paths = set()
            for entry in collection:
                safe_path(entry["path"])
                folded = entry["path"].casefold()
                if folded in paths or not isinstance(entry["size"], int) or entry["size"] < 0:
                    raise AssetError("Invalid or duplicate asset manifest entry")
                if len(entry["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in entry["sha256"]):
                    raise AssetError("Invalid asset digest")
                paths.add(folded)
        for directory in value["directories"]:
            safe_path(directory)
        xex = next(entry for entry in value["files"] if entry["path"] == "default.xex")
        if xex["sha256"] != SUPPORTED_XEX:
            raise AssetError("Wrong executable in asset manifest")
        return value
    except (KeyError, TypeError, AttributeError, StopIteration, json.JSONDecodeError) as exc:
        raise AssetError("Malformed asset manifest") from exc


def verify(root, progress, prepared=True):
    root = output_root(root)
    manifest = read_manifest(root)
    check_tree(root, manifest["files"], manifest["directories"], True)
    check_files(root, manifest["files"], progress, "verify-game")
    if prepared:
        if manifest["prepared"]["recipe_version"] != RECIPE_VERSION:
            raise AssetError("Asset preparation is out of date. Run the prepare command.")
        check_files(root / META, manifest["prepared"]["files"], progress, "verify-native")
    return manifest


def check_ready(root, progress):
    """Cheap launch check; full content verification is an explicit operation."""
    root = output_root(root)
    manifest = read_manifest(root)
    check_tree(root, manifest["files"], manifest["directories"], True)
    for entry in manifest["files"]:
        path = asset_path(root, entry["path"])
        if path.stat().st_size != entry["size"]:
            raise AssetError(f"Asset size mismatch: {path}. Run the verify command.")
    xex = next(entry for entry in manifest["files"] if entry["path"] == "default.xex")
    check_files(root, [xex], lambda *args: None, "check")
    progress("complete", 1, 1, "Imported game assets are ready")
    return manifest


def prepare_assets(game, target, files, progress, compile_shaders=False):
    """All asset-dependent native preparation belongs here, in versioned steps.

    Preserve cooked packages/texture caches for the existing engine loader.
    New renderer conversions must read verified files here and write under
    target; no shader, texture, or mesh extracted from a developer's game is
    an input shipped with the application.
    """
    source = game / GLOBAL_SHADERS
    relative_source = "native/shaders/source"
    destination = target / relative_source
    destination.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location("gow3_extract_shaders", ROOT / "tools/rendering/extract-shaders.py")
    scanner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(scanner)
    data = source.read_bytes()
    shaders = {}
    progress("prepare", 0, 1, "Extracting native shader inputs")
    for blob, info in scanner.containers(data):
        digest = hashlib.sha256(blob).hexdigest()
        offset = info.pop("offset")
        if digest in shaders:
            shaders[digest]["offsets"].append(offset)
            continue
        name = f"{digest}.{info['stage']}.bin"
        (destination / name).write_bytes(blob)
        shaders[digest] = dict(path=name, sha256=digest, offsets=[offset], **info)
    if not shaders:
        raise AssetError("No supported shader containers in the global shader cache")
    source_manifest = dict(source=GLOBAL_SHADERS, source_sha256=hashlib.sha256(data).hexdigest(),
                           status="Local native renderer inputs; not a whole-game pipeline cache",
                           shaders=list(shaders.values()))
    write_json(destination / MANIFEST, source_manifest)
    # An index provides stable, relative locations for future package/texture
    # readers without pretending the UE3 packages have been decoded already.
    write_json(target / "catalog.json", dict(version=1, assets=[
        {key: entry[key] for key in ("path", "size", "sha256", "kind")} for entry in files]))
    if compile_shaders:
        progress("compile", 0, len(shaders), "Compiling native shaders; this can take a few minutes")
        command = [sys.executable, str(ROOT / "tools/rendering/compile-gears-shaders.py"),
                   str(destination / MANIFEST), str(target / "native/shaders/compiled")]
        # Keep stdout reserved for the importer's machine-readable progress.
        with (target / "shader-compile.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            detail = (target / "shader-compile.log").read_text(encoding="utf-8")[-4000:]
            raise AssetError(f"Native shader compilation failed; assets were not published.\n{detail}")
        progress("compile", len(shaders), len(shaders), "Native shader compilation complete")
    outputs = [dict(path=path.relative_to(target).as_posix(), size=path.stat().st_size,
                    sha256=digest_file(path)) for path in sorted(target.rglob("*")) if path.is_file()]
    progress("prepare", 1, 1, f"Prepared {len(shaders)} unique global shaders")
    return dict(recipe_version=RECIPE_VERSION, files=outputs, shader_count=len(shaders),
                shaders_compiled=compile_shaders,
                shader_manifest=f"{META}/{relative_source}/{MANIFEST}",
                catalog=f"{META}/catalog.json",
                scope="Original cooked assets and global shader inputs; full UE3 asset conversion is not implemented")


def import_iso(iso, output, progress, adopt=False, compile_shaders=False):
    root = output_root(output)
    progress("inspect", 0, 1, "Checking disc filesystem and supported executable")
    with DiscImage(iso) as disc:
        identify(disc)
        progress("inspect", 1, 1, "Supported Gears of War 3 executable")
        files = [entry for entry in disc.entries if not entry.directory]
        directories = [entry.path for entry in disc.entries if entry.directory]
        total = sum(entry.size for entry in files)
        with destination_lock(root):
            if (root / META / MANIFEST).exists():
                existing = verify(root, progress)
                disc_hash = hash_disc(disc, progress)
                if existing["source"]["sha256"] != disc_hash:
                    raise AssetError("Destination was imported from a different ISO. Choose a new output directory.")
                if compile_shaders and not existing["prepared"]["shaders_compiled"]:
                    return prepare(root, progress, True, locked=True)
                progress("complete", 1, 1, "This ISO is already imported and verified")
                return existing
            has_files = root.exists() and (not root.is_dir() or any(root.iterdir()))
            if has_files and not adopt:
                raise AssetError("Destination is not empty. Use --adopt-existing to verify it against the ISO, "
                                 "or select a new --output directory. Existing files will not be overwritten.")
            if has_files:
                check_tree(root, [dict(path=entry.path) for entry in files], directories, False)
            required = (0 if has_files else total) + 64 * 1024 * 1024
            if shutil.disk_usage(root.parent).free < required:
                raise AssetError(f"Not enough space: at least {required:,} free bytes required")
            disc_hash = hash_disc(disc, progress)
            with tempfile.TemporaryDirectory(prefix=f".{root.name}.import-", dir=root.parent) as temporary:
                stage = Path(temporary) / "game"
                stage.mkdir()
                game = root if has_files else stage
                if not has_files:
                    for directory in directories:
                        (stage / directory).mkdir(parents=True, exist_ok=True)
                records, done = [], 0
                for entry in files:
                    digest = hashlib.sha256()
                    path = asset_path(game, entry.path)
                    if has_files:
                        if path.stat().st_size != entry.size:
                            raise AssetError(f"Existing file has a different size: {entry.path}")
                        with path.open("rb") as existing:
                            for chunk in disc.chunks(entry.offset, entry.size):
                                if existing.read(len(chunk)) != chunk:
                                    raise AssetError(f"Existing file differs from ISO: {entry.path}")
                                digest.update(chunk)
                                done += len(chunk)
                                progress("adopt", done, total, entry.path)
                    else:
                        with path.open("xb") as target:
                            for chunk in disc.chunks(entry.offset, entry.size):
                                target.write(chunk)
                                digest.update(chunk)
                                done += len(chunk)
                                progress("extract", done, total, entry.path)
                    records.append(dict(path=entry.path, size=entry.size, sha256=digest.hexdigest(),
                                        source_offset=entry.offset, kind=kind(entry.path)))
                metadata = stage / META
                prepared = prepare_assets(game, metadata, records, progress, compile_shaders)
                manifest = dict(format="gow3.asset-import", version=1, title_id=TITLE_ID,
                                xex_sha256=SUPPORTED_XEX,
                                imported_at=datetime.now(timezone.utc).isoformat(),
                                source=dict(format="xdvdfs", name=Path(iso).name, size=disc.size,
                                            sha256=disc_hash, partition_offset=disc.partition),
                                directories=directories, files=records, prepared=prepared)
                write_json(metadata / MANIFEST, manifest)
                # Verify bytes read back from the destination before publishing.
                check_files(game, records, progress, "verify-game")
                check_files(metadata, prepared["files"], progress, "verify-native")
                disc.unchanged()
                if has_files:
                    if (root / META).exists():
                        raise AssetError("Import metadata appeared during import; refusing to replace it")
                    metadata.rename(root / META)
                else:
                    if root.exists():
                        root.rmdir()  # Only an empty destination may be replaced.
                    stage.rename(root)
                progress("complete", 1, 1, f"Imported {len(records)} files into {root}")
                return manifest


def hash_disc(disc, progress):
    digest, done = hashlib.sha256(), 0
    for chunk in disc.chunks(0, disc.size):
        digest.update(chunk)
        done += len(chunk)
        progress("hash-iso", done, disc.size, "Recording ISO provenance")
    disc.unchanged()
    return digest.hexdigest()


def prepare(root, progress, compile_shaders=False, locked=False):
    root = output_root(root)
    if not locked:
        with destination_lock(root):
            return prepare(root, progress, compile_shaders, locked=True)
    manifest = verify(root, progress, prepared=False)
    with tempfile.TemporaryDirectory(prefix=f".{root.name}.prepare-", dir=root.parent) as temporary:
        target = Path(temporary) / META
        manifest["prepared"] = prepare_assets(root, target, manifest["files"], progress, compile_shaders)
        write_json(target / MANIFEST, manifest)
        check_files(target, manifest["prepared"]["files"], progress, "verify-native")
        # Keep the previous metadata until publication succeeds. No game file
        # is changed; failures restore the previous preparation.
        previous = Path(temporary) / "previous"
        (root / META).rename(previous)
        try:
            target.rename(root / META)
        except BaseException:
            previous.rename(root / META)
            raise
    progress("complete", 1, 1, "Asset preparation complete")
    return manifest
