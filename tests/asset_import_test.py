"""Asset import integration and malformed-disc tests using synthetic content."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from assets import importer
from assets.xdvdfs import AssetError, DiscImage, MAGIC, SECTOR, safe_path


XEX = b"synthetic executable for importer tests"
XEX_HASH = hashlib.sha256(XEX).hexdigest()
SHADER = (struct.pack(">9I", 0x102A1100, 68, 12, 0, 0, 0, 36, 0, 0)
          + struct.pack(">8I", 0, 12, 0, 0, 0, 0, 0, 0) + bytes(12))
FILES = {"default.xex": XEX, importer.GLOBAL_SHADERS: SHADER,
         "GearGame/Movies/intro.bik": b"synthetic movie", "empty.txt": b""}


def make_iso(path, files=None, partition=0):
    tree = {}
    for name, data in (files or FILES).items():
        parent = tree
        parts = name.split("/")
        for component in parts[:-1]:
            parent = parent.setdefault(component, {})
        parent[parts[-1]] = data
    image = bytearray(partition + 34 * SECTOR)

    def allocate(data):
        sector = (len(image) - partition) // SECTOR
        image.extend(data)
        image.extend(bytes((-len(data)) % SECTOR))
        return sector

    def directory(tree):
        nodes = bytearray()
        previous = None
        for name, value in tree.items():
            is_dir = isinstance(value, dict)
            if is_dir:
                sector, size = directory(value)
            else:
                sector, size = allocate(value), len(value)
            offset = len(nodes)
            if previous is not None:
                struct.pack_into("<H", nodes, previous + 2, offset // 4)
            encoded = name.encode("ascii")
            nodes.extend(struct.pack("<HHIIBB", 0, 0, sector, size, 0x10 if is_dir else 0x20, len(encoded)))
            nodes.extend(encoded)
            nodes.extend(bytes((-len(nodes)) % 4))
            previous = offset
        return allocate(nodes), len(nodes)

    sector, size = directory(tree)
    header = partition + 32 * SECTOR
    image[header:header + 20] = MAGIC
    image[header + SECTOR - 20:header + SECTOR] = MAGIC
    struct.pack_into("<II", image, header + 20, sector, size)
    path.write_bytes(image)
    return partition + sector * SECTOR


def quiet(*args):
    pass


class ImportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.iso = self.root / "my game.iso"
        self.game = self.root / "installed game"
        self.root_offset = make_iso(self.iso)
        patched = patch.object(importer, "SUPPORTED_XEX", XEX_HASH)
        patched.start()
        self.addCleanup(patched.stop)

    def run_import(self, **kwargs):
        return importer.import_iso(self.iso, self.game, quiet, **kwargs)

    def test_import_verify_prepare_and_idempotence(self):
        manifest = self.run_import()
        self.assertEqual(manifest["source"]["sha256"], hashlib.sha256(self.iso.read_bytes()).hexdigest())
        for path, data in FILES.items():
            self.assertEqual((self.game / path).read_bytes(), data)
        self.assertEqual(manifest["prepared"]["shader_count"], 1)
        shader_manifest = self.game / manifest["prepared"]["shader_manifest"]
        shader = json.loads(shader_manifest.read_text())["shaders"][0]
        self.assertEqual((shader_manifest.parent / shader["path"]).read_bytes(), SHADER)
        self.assertEqual(importer.verify(self.game, quiet), manifest)
        self.assertEqual(importer.check_ready(self.game, quiet), manifest)
        self.assertEqual(self.run_import(), manifest)
        self.assertEqual(importer.prepare(self.game, quiet)["prepared"], manifest["prepared"])
        self.assertFalse(list(self.root.glob(".installed game.*")))

    def test_wrong_game_is_rejected_before_destination_created(self):
        make_iso(self.iso, {**FILES, "default.xex": b"wrong game"})
        with self.assertRaisesRegex(AssetError, "Unsupported executable"):
            self.run_import()
        self.assertFalse(self.game.exists())
        self.assertFalse(list(self.root.glob(".installed game.*")))

    def test_adopt_preserves_original_files(self):
        for path, data in FILES.items():
            target = self.game / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        times = {path: (self.game / path).stat().st_mtime_ns for path in FILES}
        with self.assertRaisesRegex(AssetError, "not empty"):
            self.run_import()
        self.run_import(adopt=True)
        for path, modified in times.items():
            self.assertEqual((self.game / path).stat().st_mtime_ns, modified)

    def test_adopt_conflict_never_overwrites_existing_files(self):
        for path, data in FILES.items():
            target = self.game / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        target = self.game / "default.xex"
        changed = b"!" + XEX[1:]
        target.write_bytes(changed)
        with self.assertRaisesRegex(AssetError, "differs from ISO"):
            self.run_import(adopt=True)
        self.assertEqual(target.read_bytes(), changed)
        self.assertFalse((self.game / importer.META).exists())

    def test_failure_and_cancellation_do_not_publish_or_leave_lock(self):
        for exception in (AssetError("conversion failed"), KeyboardInterrupt()):
            with self.subTest(exception=type(exception).__name__), patch.object(
                    importer, "prepare_assets", side_effect=exception):
                with self.assertRaises(type(exception)):
                    self.run_import()
                self.assertFalse(self.game.exists())
                self.assertFalse(list(self.root.glob(".installed game.*")))

    def test_failed_prepare_keeps_previous_preparation(self):
        self.run_import()
        manifest = (self.game / importer.META / importer.MANIFEST).read_bytes()
        with patch.object(importer, "prepare_assets", side_effect=AssetError("failed")):
            with self.assertRaises(AssetError):
                importer.prepare(self.game, quiet)
        self.assertEqual((self.game / importer.META / importer.MANIFEST).read_bytes(), manifest)
        importer.verify(self.game, quiet)

    def test_verify_rejects_same_size_game_corruption(self):
        self.run_import()
        (self.game / "default.xex").write_bytes(b"!" + XEX[1:])
        with self.assertRaisesRegex(AssetError, "checksum mismatch"):
            importer.verify(self.game, quiet)
        with self.assertRaisesRegex(AssetError, "checksum mismatch"):
            importer.check_ready(self.game, quiet)

    def test_verify_detects_prepared_corruption_and_prepare_repairs(self):
        manifest = self.run_import()
        path = self.game / importer.META / manifest["prepared"]["files"][0]["path"]
        path.write_bytes(b"damaged output")
        with self.assertRaisesRegex(AssetError, "size mismatch"):
            importer.verify(self.game, quiet)
        importer.prepare(self.game, quiet)
        importer.verify(self.game, quiet)

    def test_rejects_unexpected_files(self):
        self.run_import()
        (self.game / "extra.xxx").write_bytes(b"unexpected")
        with self.assertRaisesRegex(AssetError, "extra="):
            importer.verify(self.game, quiet)

    def test_rejects_manifest_path_traversal(self):
        self.run_import()
        path = self.game / importer.META / importer.MANIFEST
        manifest = json.loads(path.read_text())
        manifest["files"][0]["path"] = "../../escape"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(AssetError, "Unsafe asset path"):
            importer.verify(self.game, quiet)

    def test_rejects_symlinked_assets(self):
        self.run_import()
        target = self.game / "default.xex"
        target.unlink()
        outside = self.root / "outside"
        outside.write_bytes(XEX)
        try:
            target.symlink_to(outside)
        except OSError:
            self.skipTest("Creating symlinks unavailable on this host")
        with self.assertRaisesRegex(AssetError, "regular asset"):
            importer.verify(self.game, quiet)
        self.assertEqual(outside.read_bytes(), XEX)

    def test_existing_lock_is_preserved(self):
        lock = self.root / ".installed game.import.lock"
        lock.write_text("already running")
        with self.assertRaisesRegex(AssetError, "import lock"):
            self.run_import()
        self.assertEqual(lock.read_text(), "already running")

    def test_disc_partition_offset(self):
        make_iso(self.iso, partition=0xFB20)
        self.run_import()
        self.assertEqual((self.game / "default.xex").read_bytes(), XEX)

    def mutate(self, offset, fmt, *values):
        data = bytearray(self.iso.read_bytes())
        struct.pack_into(fmt, data, offset, *values)
        self.iso.write_bytes(data)

    def test_directory_node_cycle(self):
        data = self.iso.read_bytes()
        second = struct.unpack_from("<H", data, self.root_offset + 2)[0]
        self.mutate(self.root_offset + second * 4 + 2, "<H", second)
        with self.assertRaisesRegex(AssetError, "node cycle"):
            DiscImage(self.iso)

    def test_directory_alias(self):
        # Second root entry is GearGame; redirect its table to the root table.
        data = self.iso.read_bytes()
        second = struct.unpack_from("<H", data, self.root_offset + 2)[0] * 4
        root_sector, root_size = struct.unpack_from("<II", data, 32 * SECTOR + 20)
        self.mutate(self.root_offset + second + 4, "<II", root_sector, root_size)
        with self.assertRaisesRegex(AssetError, "cycle or alias"):
            DiscImage(self.iso)

    def test_truncated_and_out_of_bounds_disc(self):
        data = self.iso.read_bytes()
        for truncated in (b"", data[:32 * SECTOR + 35], data[:self.root_offset + 15]):
            self.iso.write_bytes(truncated)
            with self.assertRaises(AssetError):
                with DiscImage(self.iso):
                    pass
        self.iso.write_bytes(data)
        self.mutate(self.root_offset + 4, "<I", 0xFFFFFFFF)
        with self.assertRaisesRegex(AssetError, "beyond the ISO"):
            DiscImage(self.iso)

    def test_duplicate_case_and_unsafe_paths(self):
        make_iso(self.iso, {"default.xex": XEX, "DEFAULT.XEX": XEX})
        with self.assertRaisesRegex(AssetError, "Duplicate disc path"):
            DiscImage(self.iso)
        for path in ("../escape", "/absolute", "C:/file", "dir\\file", "CON", "aux.txt", "x.", "x ", "a//b"):
            with self.subTest(path=path), self.assertRaises(AssetError):
                safe_path(path)

    def test_input_mutation_is_rejected(self):
        with DiscImage(self.iso) as disc:
            with self.iso.open("ab") as stream:
                stream.write(b"changed")
            with self.assertRaisesRegex(AssetError, "changed during import"):
                disc.unchanged()


if __name__ == "__main__":
    unittest.main()
