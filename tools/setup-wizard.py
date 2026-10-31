#!/usr/bin/env python3
"""Set up a game from your own copy: import the base game, then optionally DLC.

The launchers start this when the game files are missing. Run it again with
--dlc to add DLC later, or --shaders to compile the game's shaders ahead of
time on an existing install.
"""
import argparse
from pathlib import Path
import sys

from assets.games import DEFAULT_GAME, load_game
from assets.importer import ROOT, Progress
from assets.shaders import precompile
from assets.wizard import run
from assets.xdvdfs import AssetError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", default=DEFAULT_GAME, help="Game definition in tools/assets/games (default: gow3)")
    parser.add_argument("--output", type=Path, default=ROOT / "game", help="Game asset directory (default: game/)")
    parser.add_argument("--user", type=Path, default=ROOT / "user", help="User data directory (default: user/)")
    parser.add_argument("--console", action="store_true", help="Use terminal prompts instead of a window")
    parser.add_argument("--dlc", action="store_true", help="Only add DLC to an imported game")
    parser.add_argument("--shaders", action="store_true",
                        help="Only compile the game's shaders ahead of time (skips ones already compiled)")
    args = parser.parse_args()
    try:
        game = load_game(args.game)
    except AssetError as exc:
        print(exc, file=sys.stderr)
        return 1
    if args.shaders:
        try:
            print(precompile(args.output, args.user, Progress()))
            return 0
        except AssetError as exc:
            print(exc, file=sys.stderr)
            return 1
    try:
        return run(game, args.output, args.user, args.console, args.dlc)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
