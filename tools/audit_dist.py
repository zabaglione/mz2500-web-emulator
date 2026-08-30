#!/usr/bin/env python3
"""Publication audit for web_emulator/web/dist.

Checks that the distributable contains nothing that could raise a copyright
complaint:
  1. Only the expected file set (our code, wasm, the self-made game D88).
  2. No byte runs from the real MZ-2500 ROMs (when roms/mz2500 is present
     locally, scan every dist file for 32-byte windows sampled from each ROM).
  3. The D88 is bit-identical to one built from this repository's sources.
"""

from __future__ import annotations

import gzip
import hashlib
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
DIST = HERE / "web" / "dist"
REPO = HERE.parent
ROM_DIR_CANDIDATES = [REPO / "roms" / "mz2500", REPO.parent.parent.parent / "roms" / "mz2500"]

EXPECTED = {
    "index.html", "style.css", "emulator.js", "audio-worklet.js",
    "cpm-system.js", "cpm-system.json", "mz2500w.js", "mz2500w.wasm",
    "neko_can_run_demo.d88", "cpm.hdd.gz",
}


def fail(msg: str) -> None:
    print(f"AUDIT FAIL: {msg}")
    sys.exit(1)


def main() -> None:
    if not DIST.is_dir():
        fail("dist not built (run tools/build_wasm.sh)")

    names = {p.name for p in DIST.iterdir() if p.is_file()}
    unexpected = names - EXPECTED
    missing = EXPECTED - names
    if unexpected:
        fail(f"unexpected files in dist: {sorted(unexpected)}")
    if missing:
        fail(f"missing files in dist: {sorted(missing)}")
    print(f"file set OK ({len(names)} files)")

    subprocess.run(["node", str(HERE / "web" / "cpm-system.test.cjs")], check=True)

    rom_dir = next((d for d in ROM_DIR_CANDIDATES if d.is_dir()), None)
    if rom_dir is None:
        print("rom scan SKIPPED (no local roms/mz2500; nothing to compare against)")
    else:
        dist_blobs = [(p.name, p.read_bytes()) for p in sorted(DIST.iterdir()) if p.is_file()]
        # scan compressed payloads in their inflated form too - ROM bytes
        # hidden under gzip would slip past a raw byte scan
        import gzip as _gzip
        dist_blobs += [(f"{name} (inflated)", _gzip.decompress(blob))
                       for name, blob in list(dist_blobs) if name.endswith(".gz")]
        windows = 0
        for rom in sorted(rom_dir.glob("*.rom")):
            data = rom.read_bytes()
            step = max(len(data) // 512, 32)
            for off in range(0, len(data) - 32, step):
                window = data[off : off + 32]
                if len(set(window)) <= 2:
                    continue  # skip trivial fill patterns shared by any binary
                windows += 1
                for name, blob in dist_blobs:
                    if window in blob:
                        fail(f"{rom.name} bytes at +{off:#x} found inside dist/{name}")
        print(f"rom scan OK ({windows} sampled windows, no matches)")

    make_d88 = REPO / "games/neko_can_run/tools/make_d88.py"
    if make_d88.is_file():
        rebuilt = HERE / "build" / "audit_rebuild.d88"
        subprocess.run(
            [sys.executable, str(make_d88), "--demo", "--output", str(rebuilt)],
            check=True, capture_output=True)
        if rebuilt.read_bytes() != (DIST / "neko_can_run_demo.d88").read_bytes():
            fail("dist demo D88 does not match a fresh --demo build from sources")
        print("d88 provenance OK (bit-identical --demo rebuild from repo sources)")
    else:
        print("d88 provenance SKIPPED (game sources not present; standalone checkout)")

    cpm_candidates = [
        REPO / "os/cpm/build/cpm.hdd",
        REPO / "cpm-mz2500/build/cpm.hdd",
    ]
    cpm_hdd = next((path for path in cpm_candidates if path.is_file()), None)
    if cpm_hdd is None:
        fail("CP/M source HDD not found in a supported checkout layout")
    bundled = gzip.decompress((DIST / "cpm.hdd.gz").read_bytes())
    if bundled != cpm_hdd.read_bytes():
        fail(f"dist cpm.hdd.gz does not match {cpm_hdd}")

    manifest = json.loads((DIST / "cpm-system.json").read_text())
    offset = manifest["systemOffset"]
    end = offset + manifest["systemLength"]
    if manifest["hddSize"] != len(bundled):
        fail("CP/M manifest HDD size mismatch")
    if manifest["hddSha256"] != hashlib.sha256(bundled).hexdigest():
        fail("CP/M manifest HDD hash mismatch")
    if manifest["systemSha256"] != hashlib.sha256(bundled[offset:end]).hexdigest():
        fail("CP/M manifest system hash mismatch")
    banner = f"EMM/SASI port {manifest['version']}".encode()
    if banner not in bundled:
        fail("CP/M manifest version does not match the boot banner")
    print(f"cpm.hdd provenance OK ({manifest['version']}, matches {cpm_hdd})")

    print("AUDIT PASSED")


if __name__ == "__main__":
    main()
