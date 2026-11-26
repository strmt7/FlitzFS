#!/usr/bin/env python3
"""Stage a relocatable Windows volume helper in a newly owned directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

from bind_installed_amd_package import bind, bounded_file
from check_portable_cpu import WINDOWS_OS_IMPORTS, number, pe_import_names, require


APP = "flitzfs_volume_app.exe"
WINFSP = "winfsp-x64.dll"
AMD_MANIFEST = "qualification-manifest.json"
SYSTEM_IMPORTS = WINDOWS_OS_IMPORTS | {
    "shell32.dll", "user32.dll", "gdi32.dll", "ole32.dll", "pdh.dll",
    "dxgi.dll", "cfgmgr32.dll", "wintrust.dll", "credui.dll", "netapi32.dll",
    "rpcrt4.dll", "secur32.dll", "version.dll", "wldap32.dll",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inspect_binary(data, expected_delayed):
    imports, delayed = pe_import_names(data, allow_delay=True)
    unsupported = set(imports) - SYSTEM_IMPORTS
    require(imports and not unsupported, f"Unbundled Windows imports: {sorted(unsupported)}")
    require(delayed == expected_delayed, f"Unexpected delay-loaded imports: {delayed}")
    return sorted(set(imports))


def stage(app, parent, winfsp=None, winfsp_sha256=None, amd_package=None, amd_sha256=None):
    require(os.name == "nt", "Native Windows bundle required")
    require((winfsp is None) == (winfsp_sha256 is None), "WinFsp DLL and build digest must be supplied together")
    require((amd_package is None) == (amd_sha256 is None), "AMD package and approved digest must be supplied together")
    require(amd_package is None or winfsp is not None, "Installed AMD routing requires the Explorer helper")

    application = bounded_file(app, 32 * 1024**2)
    expected_delayed = [WINFSP] if winfsp else []
    app_imports = inspect_binary(application, expected_delayed)
    optional_header = number(application, 0x3c, "<I") + 24
    require(number(application, optional_header + 68, "<H") == 2, "Expected Windows GUI helper")

    sdk = None
    sdk_imports = []
    if winfsp:
        sdk = bounded_file(winfsp, 8 * 1024**2)
        require(digest(sdk) == winfsp_sha256, "WinFsp DLL differs from the helper's bound runtime")
        sdk_imports = inspect_binary(sdk, [])

    amd_names = []
    amd_manifest = None
    if amd_package:
        _, amd_names = bind(amd_package, amd_sha256)
        amd_manifest = bounded_file(amd_package / AMD_MANIFEST, 64 * 1024)
        require(digest(amd_manifest) == amd_sha256, "AMD manifest changed during bundle preparation")

    root = Path(tempfile.mkdtemp(prefix="flitzfs-volume-", dir=parent.resolve(strict=True)))
    records = []

    def stage_file(name, data):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            stream.write(data)
        records.append(dict(path=name, bytes=len(data), sha256=digest(data)))

    stage_file(APP, application)
    if sdk is not None:
        stage_file(WINFSP, sdk)
    if amd_package:
        inventory = {item["path"]: item for item in json.loads(amd_manifest)["files"]}
        inventory[AMD_MANIFEST] = {"bytes": len(amd_manifest), "sha256": amd_sha256}
        for name in amd_names:
            expected = inventory[name]
            data = bounded_file(amd_package / name, expected["bytes"])
            require(len(data) == expected["bytes"] and digest(data) == expected["sha256"],
                    f"Approved AMD artifact changed during staging: {name}")
            stage_file("amd/" + name, data)

    manifest = {
        "schema": 1,
        "development_artifact": True,
        "architecture": "AMD64",
        "entry_point": APP,
        "application_os_imports": app_imports,
        "application_delay_imports": expected_delayed,
        "winfsp_os_imports": sdk_imports,
        "approved_amd_manifest_sha256": amd_sha256,
        "files": records,
    }
    with (root / "bundle.json").open("x", encoding="utf-8", newline="\n") as report:
        json.dump(manifest, report, indent=2)
        report.write("\n")
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--parent", type=Path, required=True)
    parser.add_argument("--winfsp", type=Path)
    parser.add_argument("--winfsp-sha256")
    parser.add_argument("--amd-package", type=Path)
    parser.add_argument("--amd-manifest-sha256")
    args = parser.parse_args()
    output = stage(args.app, args.parent, args.winfsp, args.winfsp_sha256,
                   args.amd_package, args.amd_manifest_sha256)
    print(f"Volume helper bundle: {output}")


if __name__ == "__main__":
    main()
