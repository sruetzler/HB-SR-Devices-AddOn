#!/usr/bin/env python3
"""Assemble the shared CCU addon from the per-device CCU sources."""

import argparse
import gzip
import hashlib
import os
from pathlib import Path
import re
import shutil
import tarfile
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "hb-sr-devices-addon.tgz"


def build(root: Path) -> Path:
    version = (root / "addon/VERSION").read_text().strip()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
        raise ValueError("addon/VERSION must contain a numeric dotted version")
    devices = sorted((root / "devices").glob("*/ccu"))
    if not devices:
        raise ValueError("No devices/*/ccu directories found")
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    if epoch < 0:
        raise ValueError("SOURCE_DATE_EPOCH must be non-negative")
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".addon-build-", dir=dist) as tmp:
        work = Path(tmp)
        payload = work / "payload"
        shutil.copytree(root / "addon/src", payload)
        addon = payload / "addon"
        (addon / "VERSION").write_text(version + "\n")
        models = {}

        def install(source: Path, destination: Path) -> None:
            if destination.exists():
                raise ValueError(f"Duplicate package path: {destination.relative_to(payload)}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)

        for ccu in devices:
            definitions = sorted(ccu.glob("*.xml"))
            if not definitions:
                raise ValueError(f"No XML device description in {ccu}")
            for definition in definitions:
                xml = ET.parse(definition).getroot()
                if xml.tag != "device":
                    raise ValueError(f"Expected <device> in {definition}")
                for model in xml.findall("./supported_types/type"):
                    model_id = model.attrib["id"]
                    if model_id in models and models[model_id] != definition:
                        raise ValueError(f"Duplicate device type {model_id}: {definition}")
                    models[model_id] = definition
                install(definition, addon / "firmware/rftypes" / definition.name)
            for pattern in ("install_*", "uninstall_*"):
                for script in sorted(ccu.glob(pattern)):
                    install(script, addon / script.name)
            for asset in sorted((ccu / "www").rglob("*")):
                if asset.is_file():
                    install(asset, addon / "www" / asset.relative_to(ccu / "www"))

        # Normalize modes, ownership, timestamps and ordering for reproducible
        # archives. No chmod or generated XML copies in the source directories.
        archive = work / PACKAGE
        with archive.open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as gz:
                with tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                    for path in sorted(payload.rglob("*")):
                        relative = path.relative_to(payload)
                        info = tar.gettarinfo(str(path), arcname=relative.as_posix())
                        executable = (
                            relative.as_posix() == "update_script"
                            or relative.parts[0] == "rc.d"
                            or path.name.startswith(("install_", "uninstall_"))
                            or path.name in {"patchworker", "update-check.cgi"}
                        )
                        info.uid = info.gid = 0
                        info.uname = info.gname = "root"
                        info.mtime = epoch
                        info.mode = 0o755 if path.is_dir() or executable else 0o644
                        if path.is_file():
                            with path.open("rb") as content:
                                tar.addfile(info, content)
                        else:
                            tar.addfile(info)
        result = dist / PACKAGE
        archive.replace(result)

    # Generated release files keep existing GitHub update URLs working.
    legacy = root / "CCU_RM"
    (legacy / "src/addon").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(result, legacy / PACKAGE)
    (legacy / "src/addon/VERSION").write_text(version + "\n")
    digest = hashlib.sha256(result.read_bytes()).hexdigest()
    (dist / (PACKAGE + ".sha256")).write_text(f"{digest}  {PACKAGE}\n")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        archive = build(ROOT)
    except (OSError, ValueError, ET.ParseError) as error:
        parser.exit(1, f"Addon build failed: {error}\n")
    version = (ROOT / "addon/VERSION").read_text().strip()
    print(f"Addon {version}: {archive}")
    print("Generated compatibility files in CCU_RM/ for existing update URLs.")


if __name__ == "__main__":
    main()
