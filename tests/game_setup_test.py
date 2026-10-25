"""Setup wizard back end: folder imports and DLC from folders and archives, with synthetic content."""
import hashlib
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from assets import dlc, importer
from assets.games import Dlc, Game, load_game
from assets.xdvdfs import AssetError

sys.path.insert(0, str(Path(__file__).resolve().parent))
from asset_import_test import FILES, XEX_HASH, make_iso


def quiet(*args):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


PACKAGES = {"AAAA0001": b"LIVE" + b"first pack" * 100, "BBBB0002": b"LIVE" + b"second pack" * 50}
GAME = Game("test", "Test Game", "4D5308AB", "default.xex", XEX_HASH, importer.GLOBAL_SHADERS,
            ("GearGame", "GearGame/CookedXbox360", "GearGame/Movies"),
            tuple(dict(path=path, size=len(data), sha256=digest(data)) for path, data in FILES.items()),
            tuple(Dlc(f"Pack {name}", name, len(data), digest(data), "00000002") for name, data in PACKAGES.items()))


def write_tree(root, files):
    for path, data in files.items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_bytes(data)


class FolderImportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        patched = patch.object(importer, "SUPPORTED_XEX", XEX_HASH)
        patched.start()
        self.addCleanup(patched.stop)

    def test_imports_nested_folder_with_other_case_and_extra_files(self):
        source = self.root / "download" / "Gears 3"
        write_tree(source, {path.upper() if path != "default.xex" else path: data for path, data in FILES.items()})
        (source / "readme.txt").write_text("not part of the game")
        output = self.root / "game"
        manifest = importer.import_folder(self.root / "download", output, quiet, GAME)
        self.assertEqual(manifest["source"]["format"], "folder")
        for path, data in FILES.items():
            self.assertEqual((output / path).read_bytes(), data)
        self.assertFalse((output / "readme.txt").exists())
        self.assertEqual(importer.check_ready(output, quiet), manifest)
        self.assertEqual(importer.import_folder(source, output, quiet, GAME), manifest)

    def test_missing_or_changed_files_are_rejected_without_publishing(self):
        source = self.root / "source"
        write_tree(source, {path: data for path, data in FILES.items() if path != "GearGame/Movies/intro.bik"})
        with self.assertRaisesRegex(AssetError, "missing or have the wrong size"):
            importer.import_folder(source, self.root / "game", quiet, GAME)
        write_tree(source, {"GearGame/Movies/intro.bik": b"synthetic movif"})
        with self.assertRaisesRegex(AssetError, "checksum differs"):
            importer.import_folder(source, self.root / "game", quiet, GAME)
        self.assertFalse((self.root / "game").exists())
        self.assertFalse(list(self.root.glob(".game.*")))

    def test_existing_extraction_is_adopted_in_place(self):
        output = self.root / "game"
        write_tree(output, FILES)
        manifest = importer.import_folder(output, output, quiet, GAME)
        self.assertEqual(importer.check_ready(output, quiet), manifest)

    def test_import_source_accepts_iso_and_folder(self):
        iso = self.root / "game.iso"
        make_iso(iso)
        importer.import_source(iso, self.root / "from-iso", quiet, GAME)
        importer.import_source(self.root / "from-iso", self.root / "from-folder", quiet, GAME)
        self.assertEqual(importer.check_ready(self.root / "from-folder", quiet)["source"]["format"], "folder")


class DlcTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.user = self.root / "user"

    def installed(self):
        return sorted(path.name for path in (self.user / "dlc").iterdir())

    def test_folder_zip_and_tar_sources(self):
        folder = self.root / "folder" / "4D5308AB" / "00000002"
        write_tree(folder, {"AAAA0001": PACKAGES["AAAA0001"]})
        archive = self.root / "packs.zip"
        with zipfile.ZipFile(archive, "w") as out:
            out.writestr("4D5308AB/00000002/BBBB0002", PACKAGES["BBBB0002"])
        found = dlc.find([self.root / "folder", archive], GAME)
        self.assertEqual([item.package.file for item in found], ["AAAA0001", "BBBB0002"])
        self.assertEqual(dlc.install(found, GAME, self.user, quiet), ["Pack AAAA0001", "Pack BBBB0002"])
        self.assertEqual(self.installed(), ["AAAA0001", "BBBB0002"])
        for name, data in PACKAGES.items():
            self.assertEqual((self.user / "dlc" / name).read_bytes(), data)
        self.assertEqual(dlc.install(dlc.find([archive], GAME), GAME, self.user, quiet), [])

    def test_tar_xz_and_archives_inside_folders(self):
        staging = self.root / "staging"
        write_tree(staging, {f"content/{name}": data for name, data in PACKAGES.items()})
        archive = self.root / "downloads" / "packs.tar.xz"
        archive.parent.mkdir()
        with tarfile.open(archive, "w:xz") as out:
            out.add(staging / "content", arcname="content")
        found = dlc.find([self.root / "downloads"], GAME)
        self.assertEqual(len(found), 2)
        dlc.install(found, GAME, self.user, quiet)
        self.assertEqual(self.installed(), ["AAAA0001", "BBBB0002"])

    def test_wrong_contents_are_rejected(self):
        folder = self.root / "folder"
        write_tree(folder, {"AAAA0001": b"LIVE" + b"first pacK" * 100})
        with self.assertRaisesRegex(AssetError, "does not match"):
            dlc.install(dlc.find([folder], GAME), GAME, self.user, quiet)
        self.assertEqual(self.installed(), [])

    def test_unpacked_package_counts_as_installed(self):
        header = self.user / "0000000000000000" / "4D5308AB" / "Headers" / "00000002" / "AAAA0001.header"
        header.parent.mkdir(parents=True)
        header.write_bytes(b"")
        self.assertTrue(dlc.installed(GAME.dlc[0], GAME, self.user))
        self.assertFalse(dlc.installed(GAME.dlc[1], GAME, self.user))


class GameDefinitionTest(unittest.TestCase):
    def test_gears_of_war_3_definition(self):
        game = load_game("gow3")
        self.assertEqual(game.title_id, "4D5308AB")
        self.assertEqual(game.base_file("DEFAULT.XEX")["sha256"], game.xex_sha256)
        self.assertEqual(len(game.dlc), 5)
        self.assertEqual(game.dlc_package("a88137aa80ac7cc1560e7f1e820c090c21509a834d").name, "Horde Command Pack")


if __name__ == "__main__":
    unittest.main()
