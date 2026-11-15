"""Packaged helper entry point; ancillary tools do not change game-code generation."""
import sys


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--steam-shortcut':
        from steam_shortcut import main as shortcut_main
        return shortcut_main(sys.argv[2:])
    from gen import main as generate
    generate()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
