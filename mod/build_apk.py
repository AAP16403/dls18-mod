"""Repack DLS18 APK with patched libraries and optional animation KPX, then v1-sign it.

targetSdk 27 / minSdk 19 -> JAR signing with SHA-256 is sufficient on every Android version.
Signing key is generated once (self-signed, mod/keys/) with WSL's openssl.

usage: python build_apk.py [--base-apk original/DLS18_5.064.apk]
                           [--lib build/lib/armeabi-v7a/libDLS18.so]
                           [--career-lib build/lib/armeabi-v7a/libCareerMarket.so]
                           [--anims-pak build/anims_reworked.pak]
                           [--out build/DLS18_sprintmod.apk]
"""
import argparse
import base64
import hashlib
import re
import subprocess
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SRC_APK = ROOT / "original/DLS18_5.064.apk"
KEYS = HERE / "keys"
SIG_RE = re.compile(r"^META-INF/([^/]+\.(MF|SF|RSA|DSA|EC)|SIG-.*)$", re.I)
CREATED = "Created-By: 1.0 (DLS18 sprint mod)"


def wsl_path(p: Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args, stdin=None):
    r = subprocess.run(["wsl", "-e", *args], input=stdin, capture_output=True)
    if r.returncode:
        raise SystemExit(f"{' '.join(args)} failed:\n{r.stderr.decode(errors='replace')}")
    return r.stdout


def ensure_key():
    KEYS.mkdir(exist_ok=True)
    key, cert = KEYS / "mod.key.pem", KEYS / "mod.cert.pem"
    if not key.exists():
        wsl("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "10000",
            "-subj", "/CN=DLS18 Sprint Mod", "-keyout", wsl_path(key), "-out", wsl_path(cert))
    return key, cert


def wrap(line: str) -> bytes:
    """Manifest line wrapping: 72-byte lines, continuation lines start with a space."""
    b = line.encode()
    out = [b[:70]]
    b = b[70:]
    while b:
        out.append(b" " + b[:69])
        b = b[69:]
    return b"\r\n".join(out) + b"\r\n"


def b64sha(data: bytes) -> str:
    return base64.b64encode(hashlib.sha256(data).digest()).decode()


def sign_v1(entries):
    """entries: list of (name, data). Returns {MANIFEST.MF, CERT.SF, CERT.RSA} bytes."""
    man = wrap("Manifest-Version: 1.0") + wrap(CREATED) + b"\r\n"
    sf_sections = []
    for name, data in entries:
        sec = wrap(f"Name: {name}") + wrap(f"SHA-256-Digest: {b64sha(data)}") + b"\r\n"
        man += sec
        sf_sections.append(wrap(f"Name: {name}") + wrap(f"SHA-256-Digest: {b64sha(sec)}") + b"\r\n")
    main_attr = wrap("Manifest-Version: 1.0") + wrap(CREATED) + b"\r\n"
    sf = (wrap("Signature-Version: 1.0") + wrap(CREATED)
          + wrap(f"SHA-256-Digest-Manifest: {b64sha(man)}")
          + wrap(f"SHA-256-Digest-Manifest-Main-Attributes: {b64sha(main_attr)}") + b"\r\n"
          + b"".join(sf_sections))
    key, cert = ensure_key()
    rsa = wsl("openssl", "smime", "-sign", "-binary", "-noattr", "-outform", "DER", "-md", "sha256",
              "-signer", wsl_path(cert), "-inkey", wsl_path(key), stdin=sf)
    return {"META-INF/MANIFEST.MF": man, "META-INF/CERT.SF": sf, "META-INF/CERT.RSA": rsa}


def write_aligned(zout, info, data, align):
    """Pad the local-header extra field so stored data starts on an `align` boundary."""
    info.extra = b""
    if info.compress_type == zipfile.ZIP_STORED:
        hdr = 30 + len(info.filename.encode())
        pad = (-(zout.fp.tell() + hdr)) % align
        info.extra = b"\x00" * pad
    zout.writestr(info, data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-apk", default=str(SRC_APK),
                    help="APK to preserve assets/resources from (default: original game APK)")
    ap.add_argument("--lib", default=str(HERE / "build/lib/armeabi-v7a/libDLS18.so"))
    ap.add_argument("--career-lib", help="include libCareerMarket.so in the ARMv7 library directory")
    ap.add_argument("--anims-pak", help="replace assets/data/anims/anims.pak with this edited KPX archive")
    ap.add_argument("--asset", action="append", default=[], metavar="APK_PATH=FILE",
                    help="replace any APK entry (repeatable), e.g. assets/data/nis.pak=build/unlock/nis.pak")
    ap.add_argument("--out", default=str(HERE / "build/DLS18_sprintmod.apk"))
    a = ap.parse_args()
    extra_assets = {}
    for spec in a.asset:
        name, _, path = spec.partition("=")
        if not name or not path:
            raise SystemExit(f"--asset needs APK_PATH=FILE, got {spec!r}")
        extra_assets[name] = Path(path).read_bytes()
    newlib = Path(a.lib).read_bytes()
    new_career_lib = Path(a.career_lib).read_bytes() if a.career_lib else None
    new_anims = Path(a.anims_pak).read_bytes() if a.anims_pak else None

    zin = zipfile.ZipFile(a.base_apk)
    items = []
    saw_career_lib = False
    for info in zin.infolist():
        if SIG_RE.match(info.filename) or info.is_dir():
            continue
        if info.filename == "lib/armeabi-v7a/libDLS18.so":
            data = newlib
        elif info.filename == "lib/armeabi-v7a/libCareerMarket.so":
            saw_career_lib = True
            data = new_career_lib if new_career_lib is not None else zin.read(info)
        elif info.filename in extra_assets:
            data = extra_assets.pop(info.filename)
        elif info.filename == "assets/data/anims/anims.pak" and new_anims is not None:
            data = new_anims
        else:
            data = zin.read(info)
        items.append((info, data))
    if extra_assets:
        raise SystemExit(f"--asset entries not found in the base APK: {', '.join(extra_assets)}")
    if new_career_lib is not None and not saw_career_lib:
        info = zipfile.ZipInfo("lib/armeabi-v7a/libCareerMarket.so", date_time=(2018, 5, 22, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        items.append((info, new_career_lib))

    sigs = sign_v1([(i.filename, d) for i, d in items])
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w") as zout:
        for name in ("META-INF/MANIFEST.MF", "META-INF/CERT.SF", "META-INF/CERT.RSA"):
            zi = zipfile.ZipInfo(name, date_time=(2018, 5, 22, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            write_aligned(zout, zi, sigs[name], 4)
        for info, data in items:
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.external_attr = info.external_attr
            write_aligned(zout, zi, data, 4096 if info.filename.endswith(".so") else 4)
    # self-check: every entry readable, lib matches
    with zipfile.ZipFile(out) as z:
        assert z.testzip() is None
        assert z.read("lib/armeabi-v7a/libDLS18.so") == newlib
        if new_career_lib is not None:
            assert z.read("lib/armeabi-v7a/libCareerMarket.so") == new_career_lib
        if new_anims is not None:
            assert z.read("assets/data/anims/anims.pak") == new_anims
    print(f"wrote {out} ({out.stat().st_size:,} bytes, {len(items)} entries + v1 signature)")


if __name__ == "__main__":
    main()
