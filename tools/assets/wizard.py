"""First-run setup: import the user's own copy of the game, then optionally its DLC.

The wizard is driven by the game definition (tools/assets/games/<id>.json):
its title, the files its disc provides and its known DLC packages. A Qt
window is used when PySide6 and a display are available, otherwise prompts
on the terminal.
"""
import os
from pathlib import Path
import sys
import threading

from . import dlc
from .importer import Progress, check_ready, import_source
from .xdvdfs import AssetError


def base_ready(game_root):
    try:
        check_ready(game_root, lambda *args: None)
        return True
    except (AssetError, OSError):
        return False


def _console_progress():
    progress = Progress()
    return lambda phase, done, total, message: progress(phase, done, total, message[-60:])


def _ask(prompt):
    try:
        return input(prompt).strip().strip('"').strip("'")
    except EOFError:
        return ""


def run_console(game, game_root, user_root, dlc_only=False):
    print(f"\n{game.title} setup\n")
    if not dlc_only and not base_ready(game_root):
        print(f"squarehole360 needs your own copy of {game.title}: an Xbox 360 disc image (ISO)\n"
              "or a folder with the extracted disc files.")
        while True:
            source = _ask("Path to the ISO or folder (empty to cancel): ")
            if not source:
                return 1
            try:
                import_source(Path(source).expanduser(), game_root, _console_progress(), game)
                break
            except (AssetError, OSError) as exc:
                print(f"\nCould not import: {exc}\n")
    if not base_ready(game_root):
        print(f"{game.title} is not imported yet; import the base game first.")
        return 1
    answer = _ask("\nAdd downloadable content (DLC)? [y/N] ").lower()
    while answer.startswith("y"):
        source = _ask("Folder or archive (.zip, .tar.gz, .tar.xz) with the DLC (empty to finish): ")
        if not source:
            break
        try:
            found = dlc.find([Path(source)], game)
            if not found:
                print(f"No {game.title} DLC found there.")
                continue
            for item in found:
                print(f"  found {item.package.name}")
            dlc.install(found, game, user_root, _console_progress())
        except (AssetError, OSError) as exc:
            print(f"\nCould not add DLC: {exc}\n")
    print("\nSetup complete.")
    return 0


def run_gui(game, game_root, user_root, dlc_only=False):
    from PySide6.QtCore import QObject, Qt, Signal
    from PySide6.QtWidgets import (QApplication, QButtonGroup, QFileDialog, QHBoxLayout, QLabel, QLineEdit,
                                   QListWidget, QListWidgetItem, QProgressBar, QPushButton, QRadioButton,
                                   QVBoxLayout, QWizard, QWizardPage)

    class Worker(QObject):
        progress = Signal(str, int, int, str)
        finished = Signal(object)

        def start(self, work):
            def run():
                try:
                    result = work(lambda phase, done, total, message: self.progress.emit(phase, done, total, message))
                    self.finished.emit(None if result is not False else "Cancelled")
                except (AssetError, OSError) as exc:
                    self.finished.emit(str(exc))
            threading.Thread(target=run, daemon=True).start()

    app = QApplication.instance() or QApplication(sys.argv)
    wizard = QWizard()
    wizard.setWindowTitle(f"{game.title} setup")
    wizard.setWizardStyle(QWizard.ModernStyle)
    wizard.setOption(QWizard.NoBackButtonOnLastPage)
    wizard.resize(720, 460)
    state = dict(dlc_found=[], installed=[])

    def phase_text(phase):
        return {"inspect": "Checking", "hash-iso": "Reading the disc", "extract": "Extracting",
                "copy": "Copying", "verify": "Verifying", "verify-game": "Verifying", "verify-native": "Verifying",
                "prepare": "Preparing", "adopt": "Verifying", "dlc": "Installing DLC"}.get(phase, phase.capitalize())

    class WorkPage(QWizardPage):
        """Runs one job with a progress bar; Next is enabled once it succeeds."""
        def __init__(self, title, subtitle, job):
            super().__init__()
            self.setTitle(title)
            self.setSubTitle(subtitle)
            self.job, self.done, self.running = job, False, False
            layout = QVBoxLayout(self)
            self.bar, self.status = QProgressBar(), QLabel()
            self.status.setWordWrap(True)
            layout.addWidget(self.bar)
            layout.addWidget(self.status)
            layout.addStretch()
            self.worker = Worker()
            self.worker.progress.connect(self.on_progress)
            self.worker.finished.connect(self.on_finished)

        def initializePage(self):
            self.done, self.running = False, True
            self.bar.setRange(0, 0)
            self.status.setText("Starting...")
            self.completeChanged.emit()
            self.worker.start(self.job)

        def on_progress(self, phase, done, total, message):
            self.bar.setRange(0, 1000)
            self.bar.setValue(int(1000 * done / total) if total else 0)
            self.status.setText(f"{phase_text(phase)}: {message}")

        def on_finished(self, error):
            self.running = False
            self.bar.setRange(0, 1000)
            if error:
                self.bar.setValue(0)
                self.status.setText(f"<b>Failed:</b> {error}<br><br>Go back to choose another source.")
            else:
                self.done = True
                self.bar.setValue(1000)
            self.completeChanged.emit()

        def isComplete(self):
            return self.done

        def validatePage(self):
            return self.done

    # Base game source.
    source_page = QWizardPage()
    source_page.setTitle(f"Your copy of {game.title}")
    source_page.setSubTitle("squarehole360 includes no game files. Choose your Xbox 360 disc image (ISO) "
                            "or a folder with the extracted disc files.")
    source_edit = QLineEdit()
    source_edit.setPlaceholderText("Path to an ISO file or a game folder")
    iso_button, folder_button = QPushButton("Choose ISO..."), QPushButton("Choose folder...")
    iso_button.clicked.connect(lambda: source_edit.setText(QFileDialog.getOpenFileName(
        wizard, "Disc image", str(Path.home()), "Disc images (*.iso *.xiso);;All files (*)")[0] or source_edit.text()))
    folder_button.clicked.connect(lambda: source_edit.setText(QFileDialog.getExistingDirectory(
        wizard, "Extracted game folder", str(Path.home())) or source_edit.text()))
    row = QHBoxLayout()
    row.addWidget(source_edit, 1)
    row.addWidget(iso_button)
    row.addWidget(folder_button)
    layout = QVBoxLayout(source_page)
    layout.addLayout(row)
    note = QLabel(f"Every file is checked against the known {game.title} files before it is used.")
    note.setWordWrap(True)
    layout.addWidget(note)
    layout.addStretch()
    source_page.isComplete = lambda: bool(source_edit.text().strip()) and Path(source_edit.text().strip()).expanduser().exists()
    source_edit.textChanged.connect(source_page.completeChanged)

    import_page = WorkPage(f"Importing {game.title}", "This copies and verifies the game files; it can take a few minutes.",
                           lambda progress: import_source(Path(source_edit.text().strip()).expanduser(), game_root,
                                                          progress, game))

    # DLC question and sources.
    ask_page = QWizardPage()
    ask_page.setTitle("Downloadable content")
    ask_page.setSubTitle(f"Do you want to add {game.title} DLC? You can also do this later.")
    yes, no = QRadioButton("Yes, add DLC from folders or archives"), QRadioButton("No, finish setup")
    no.setChecked(True)
    group = QButtonGroup(ask_page)
    group.addButton(yes)
    group.addButton(no)
    known = QLabel("Known packages: " + ", ".join(package.name for package in game.dlc))
    known.setWordWrap(True)
    layout = QVBoxLayout(ask_page)
    for widget in (yes, no, known):
        layout.addWidget(widget)
    layout.addStretch()

    dlc_page = QWizardPage()
    dlc_page.setTitle("Add DLC")
    dlc_page.setSubTitle("Add folders or archives (.zip, .tar.gz, .tar.xz) that contain the DLC packages.")
    found_list = QListWidget()
    status = QLabel()
    status.setWordWrap(True)
    sources = []

    def rescan(paths):
        paths = [Path(path) for path in ([paths] if isinstance(paths, str) else paths) if path]
        if not paths:
            return
        status.setText("Searching...")
        app.processEvents()
        failed = []
        for path in paths:
            try:
                dlc.find([path], game)
                sources.append(path)
            except (AssetError, OSError) as exc:
                failed.append(f"{path.name}: {exc}")
        state["dlc_found"] = dlc.find(sources, game) if sources else []
        status.setText(f"{len(state['dlc_found'])} of {len(game.dlc)} known packages found."
                       + (" Could not read " + "; ".join(failed) if failed else ""))
        found_list.clear()
        for item in state["dlc_found"]:
            mark = " (already installed)" if dlc.installed(item.package, game, user_root) else ""
            entry = QListWidgetItem(f"{item.package.name}{mark}  -  {item.source}")
            entry.setFlags(entry.flags() & ~Qt.ItemIsSelectable)
            found_list.addItem(entry)
        dlc_page.completeChanged.emit()

    add_folder, add_archive = QPushButton("Add folder..."), QPushButton("Add archives...")
    add_folder.clicked.connect(lambda: rescan(QFileDialog.getExistingDirectory(wizard, "DLC folder", str(Path.home()))))
    add_archive.clicked.connect(lambda: rescan(QFileDialog.getOpenFileNames(
        wizard, "DLC archives", str(Path.home()),
        "Archives (*.zip *.tar *.tar.gz *.tgz *.tar.xz *.txz *.tar.bz2);;All files (*)")[0]))
    buttons = QHBoxLayout()
    buttons.addWidget(add_folder)
    buttons.addWidget(add_archive)
    buttons.addStretch()
    layout = QVBoxLayout(dlc_page)
    layout.addLayout(buttons)
    layout.addWidget(found_list, 1)
    layout.addWidget(status)
    dlc_page.isComplete = lambda: bool(state["dlc_found"])

    def install_job(progress):
        state["installed"] = dlc.install(state["dlc_found"], game, user_root, progress)
    install_page = WorkPage("Installing DLC", "The game unpacks new DLC once, the next time it starts.", install_job)

    done_page = QWizardPage()
    done_page.setTitle("Setup complete")
    done_label = QLabel()
    done_label.setWordWrap(True)
    QVBoxLayout(done_page).addWidget(done_label)
    done_page.initializePage = lambda: done_label.setText(
        f"{game.title} is ready." + (f" Added DLC: {', '.join(state['installed'])}." if state["installed"] else ""))

    pages = {}
    for page in (source_page, import_page, ask_page, dlc_page, install_page, done_page):
        pages[page] = wizard.addPage(page)
    ids = {page: pages[page] for page in pages}
    start = ids[ask_page] if dlc_only or base_ready(game_root) else ids[source_page]
    wizard.setStartId(start)

    def next_id(page):
        if page is ask_page:
            return ids[dlc_page] if yes.isChecked() else ids[done_page]
        order = [source_page, import_page, ask_page, dlc_page, install_page, done_page]
        index = order.index(page)
        return ids[order[index + 1]] if index + 1 < len(order) else -1
    for page in pages:
        page.nextId = (lambda page=page: next_id(page))
    wizard.exec()
    return 0 if base_ready(game_root) else 1


def run(game, game_root, user_root, console=False, dlc_only=False):
    graphical = not console and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or os.name == "nt")
    if graphical:
        try:
            import PySide6  # noqa: F401
        except ImportError:
            graphical = False
    if graphical:
        return run_gui(game, game_root, user_root, dlc_only)
    if not sys.stdin.isatty():
        print(f"{game.title} is not set up. Run tools/setup-wizard.py in a terminal or install PySide6.",
              file=sys.stderr)
        return 1
    return run_console(game, game_root, user_root, dlc_only)
