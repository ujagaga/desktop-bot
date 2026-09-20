#!/usr/bin/env python3
"""Generate VS Code configuration from Arduino's successful S3 build."""
import json
from pathlib import Path
import shlex

ROOT = Path(__file__).resolve().parent.parent
SKETCH = ROOT / "ESP32_S3"
CACHE = SKETCH / ".cache"
VSCODE = ROOT / ".vscode"


def expand(arguments, directory):
    result = []
    for arg in arguments:
        if arg.startswith("@"):
            path = Path(arg[1:])
            if not path.is_absolute():
                path = directory / path
            result.extend(expand(shlex.split(path.read_text()), directory))
        else:
            result.append(arg)
    return result


def normalize(arguments):
    # Turn GCC's prefixed SDK paths into ordinary includes understood by editors.
    result = []
    prefix = ""
    it = iter(arguments)
    for arg in it:
        if arg == "-iprefix":
            prefix = next(it)
        elif arg == "-iwithprefixbefore":
            result.append("-I" + prefix + next(it))
        elif arg.startswith("-iwithprefixbefore"):
            result.append("-I" + prefix + arg[len("-iwithprefixbefore"):])
        else:
            result.append(arg)
    return result


def main():
    entries = json.loads((CACHE / "compile_commands.json").read_text())
    output = []
    source_entries = []
    for entry in entries:
        args = normalize(expand(entry["arguments"], Path(entry["directory"])))
        # The SDK lists a few optional directories absent from this package.
        args = [a for a in args if not a.startswith("-I") or
                (Path(entry["directory"]) / a[2:]).is_dir()]
        output.append(dict(entry, arguments=args))
        path = Path(entry["file"])
        if path.parent != CACHE / "sketch":
            continue
        source = SKETCH / path.name
        if path.name.endswith(".ino.cpp"):
            source = SKETCH / path.name[:-4]
        if source.exists():
            mapped = dict(entry, file=str(source),
                          arguments=[str(source) if a == str(path) else a for a in args])
            output.append(mapped)
            source_entries.append(mapped)
    if not source_entries:
        raise RuntimeError("No sketch commands found; run tools/build_s3.sh first")
    VSCODE.mkdir(exist_ok=True)
    (VSCODE / "compile_commands.json").write_text(json.dumps(output, indent=2) + "\n")
    args = source_entries[0]["arguments"]
    config = {
        "name": "ESP32-S3",
        "compileCommands": "${workspaceFolder}/.vscode/compile_commands.json",
        "compilerPath": args[0],
        "includePath": list(dict.fromkeys(a[2:] for a in args if a.startswith("-I"))),
        "defines": [a[2:] for a in args if a.startswith("-D")],
        "intelliSenseMode": "linux-gcc-x86",
        "cStandard": "gnu17",
        "cppStandard": "gnu++23",
    }
    (VSCODE / "c_cpp_properties.json").write_text(
        json.dumps({"configurations": [config], "version": 4}, indent=2) + "\n")
    print(f"Updated ESP32-S3 IntelliSense for {len(source_entries)} source files")


if __name__ == "__main__":
    main()
