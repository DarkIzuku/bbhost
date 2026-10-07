"""Public build facts only: no environment dump, account, game or signing keys."""
import json
import pathlib
import subprocess
import sys


def version(*args):
    return subprocess.check_output(args, text=True).strip()


info = {
    "repository": "DarkIzuku/bbhost",
    "commit": version("git", "rev-parse", "HEAD"),
    "upstream_baseline": "fa904a4f9cab3753f2ec7d258cd8d271b99f6166",
    "workflow": "windows-integration.yml",
    "target": "Windows x86-64",
    "compiler": version("clang", "--version"),
    "cmake": version("cmake", "--version"),
    "mingw": version("x86_64-w64-mingw32-g++", "--version"),
    "windows_tests": "tools/win_tests.sh (Wine, after build)",
}
pathlib.Path(sys.argv[1]).write_text(json.dumps(info, indent=2) + "\n")
