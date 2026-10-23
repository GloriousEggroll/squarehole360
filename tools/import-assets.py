#!/usr/bin/env python3
"""Import a user-provided Gears of War 3 Xbox 360 ISO for this game build."""
import argparse
from pathlib import Path
import sys

from assets.importer import ROOT, Progress, check_ready, import_iso, prepare, verify
from assets.xdvdfs import AssetError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    extract = sub.add_parser("import", help="Extract and prepare assets from your own ISO")
    extract.add_argument("iso", type=Path)
    extract.add_argument("--adopt-existing", action="store_true",
                         help="Verify an existing extraction against the ISO and prepare it without replacing its files")
    validate = sub.add_parser("verify", help="Verify installed game assets and prepared renderer data")
    check = sub.add_parser("check", help="Fast launch check: file inventory/sizes and executable hash")
    update = sub.add_parser("prepare", help="Rebuild native data from previously imported game assets")
    for command in (extract, validate, check, update):
        command.add_argument("--output", type=Path, default=ROOT / "game", help="Game asset directory (default: game/)")
        command.add_argument("--json-progress", action="store_true", help="Emit JSON lines for a setup frontend")
    for command in (extract, update):
        command.add_argument("--compile-shaders", action="store_true",
                             help="Also compile global shaders using the local XenosRecomp/DXC tools")
    args = parser.parse_args()
    progress = Progress(args.json_progress)
    try:
        if args.command == "import":
            import_iso(args.iso, args.output, progress, args.adopt_existing, args.compile_shaders)
        elif args.command == "verify":
            verify(args.output, progress)
            progress("complete", 1, 1, "All imported and prepared assets verified")
        elif args.command == "check":
            check_ready(args.output, progress)
        else:
            prepare(args.output, progress, args.compile_shaders)
    except (AssetError, OSError, RuntimeError) as exc:
        progress("error", 0, 1, str(exc))
        return 1
    except KeyboardInterrupt:
        progress("cancelled", 0, 1, "Import cancelled")
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
