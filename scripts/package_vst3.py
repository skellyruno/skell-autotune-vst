#!/usr/bin/env python3
"""Validate and package a Windows VST3 engineering preview without dependencies."""
from __future__ import annotations
import argparse, hashlib, json, os, shutil, struct, sys, tempfile, zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

EXPECTED_CATEGORIES = {"Audio Module Class", "Component Controller Class", "Plugin Compatibility Class"}

class PackageError(RuntimeError):
    pass

def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def strip_trailing_commas(text: str) -> str:
    """Remove JSON trailing commas without touching commas inside strings."""
    out, in_string, escaped, i = [], False, False, 0
    while i < len(text):
        char = text[i]
        if in_string:
            out.append(char)
            if escaped: escaped = False
            elif char == "\\": escaped = True
            elif char == '"': in_string = False
            i += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            i += 1
            continue
        if char == ",":
            lookahead = i + 1
            while lookahead < len(text) and text[lookahead].isspace(): lookahead += 1
            if lookahead < len(text) and text[lookahead] in "}]":
                i += 1
                continue
        out.append(char)
        i += 1
    return "".join(out)

def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result = {}
    for key, value in pairs:
        if key in result: raise PackageError(f"duplicate JSON key: {key}")
        result[key] = value
    return result

def _reject_nonfinite(value: str):
    raise PackageError(f"non-finite JSON value is not permitted: {value}")

def load_strict_json(raw: bytes, label: str):
    try: text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc: raise PackageError(f"{label} is not UTF-8: {exc}") from exc
    normalized = strip_trailing_commas(text)
    try:
        value = json.loads(normalized, object_pairs_hook=_unique_object, parse_constant=_reject_nonfinite)
    except (json.JSONDecodeError, PackageError) as exc:
        raise PackageError(f"{label} is not valid strict JSON: {exc}") from exc
    encoded = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    return value, encoded

def load_identity(path: Path) -> dict[str, Any]:
    identity, _ = load_strict_json(path.read_bytes(), str(path))
    if not isinstance(identity, dict): raise PackageError("release identity must be a JSON object")
    required = ("product_name", "version", "vendor", "manufacturer_code", "plugin_code", "class_ids", "owner_approved", "vendor_url", "support_email", "license_status", "third_party_notices_status", "eula_status", "trademark_status")
    missing = [key for key in required if key not in identity]
    if missing: raise PackageError("release identity missing fields: " + ", ".join(missing))
    return identity

def check_release_approval(identity: dict[str, Any]) -> None:
    failures = []
    if identity.get("owner_approved") is not True: failures.append("owner_approved is not true")
    if not str(identity.get("vendor_url", "")).strip(): failures.append("vendor_url is missing")
    if not str(identity.get("support_email", "")).strip(): failures.append("support_email is missing")
    expected = {"license_status": "approved", "third_party_notices_status": "complete", "eula_status": "approved", "trademark_status": "cleared"}
    for field, value in expected.items():
        if identity.get(field) != value: failures.append(f"{field} is not {value!r}")
    if failures: raise PackageError("public release blocked: " + "; ".join(failures))

def validate_cmake_identity(cmake_path: Path, identity: dict[str, Any]) -> None:
    import re
    text = cmake_path.read_text(encoding="utf-8")
    patterns = {
        "version": r"project\s*\(\s*HumHouseVocalTune\s+VERSION\s+([^\s)]+)",
        "vendor": r"COMPANY_NAME\s+\"([^\"]+)\"",
        "manufacturer_code": r"PLUGIN_MANUFACTURER_CODE\s+([A-Za-z0-9]{4})",
        "plugin_code": r"PLUGIN_CODE\s+([A-Za-z0-9]{4})",
        "product_name": r"PRODUCT_NAME\s+\"([^\"]+)\"",
    }
    for field, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match: raise PackageError(f"CMake identity field not found: {field}")
        if match.group(1) != str(identity[field]):
            raise PackageError(f"CMake {field} {match.group(1)!r} does not match release identity {identity[field]!r}")

def inspect_pe_x64(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ": raise PackageError(f"VST3 module is not a PE executable: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 26 > len(data) or data[pe_offset:pe_offset+4] != b"PE\0\0": raise PackageError(f"invalid PE header: {path}")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    optional_magic = struct.unpack_from("<H", data, pe_offset + 24)[0]
    if machine != 0x8664 or optional_magic != 0x20B:
        raise PackageError(f"VST3 module must be AMD64 PE32+ (machine=0x{machine:04x}, optional=0x{optional_magic:04x})")
    return {"machine": "AMD64", "format": "PE32+", "size_bytes": len(data), "sha256": sha256_file(path)}

def validate_bundle(bundle: Path, identity: dict[str, Any]) -> dict[str, Any]:
    if not bundle.is_dir() or bundle.suffix.lower() != ".vst3": raise PackageError(f"expected a VST3 bundle directory, got: {bundle}")
    moduleinfo = bundle / "Contents" / "Resources" / "moduleinfo.json"
    if not moduleinfo.is_file(): raise PackageError("VST3 bundle is missing Contents/Resources/moduleinfo.json")
    original = moduleinfo.read_bytes()
    metadata, normalized = load_strict_json(original, "moduleinfo.json")
    if not isinstance(metadata, dict): raise PackageError("moduleinfo.json root must be an object")
    factory = metadata.get("Factory Info")
    if not isinstance(factory, dict): raise PackageError("moduleinfo.json is missing Factory Info")
    for key, value in (("Name", identity["product_name"]), ("Version", identity["version"])):
        if metadata.get(key) != value: raise PackageError(f"moduleinfo {key} {metadata.get(key)!r} does not match {value!r}")
    if factory.get("Vendor") != identity["vendor"]: raise PackageError("moduleinfo Factory Info Vendor does not match release identity")
    classes = metadata.get("Classes")
    if not isinstance(classes, list) or len(classes) != 3: raise PackageError("expected exactly three VST3 classes (processor, controller, compatibility)")
    categories = [item.get("Category") for item in classes if isinstance(item, dict)]
    if len(categories) != 3 or set(categories) != EXPECTED_CATEGORIES: raise PackageError("VST3 class categories do not match the expected processor/controller/compatibility set")
    cids = [item.get("CID") for item in classes]
    if any(not isinstance(cid, str) or len(cid) != 32 or any(c not in "0123456789abcdefABCDEF" for c in cid) for cid in cids): raise PackageError("VST3 class IDs must be 32-character hexadecimal strings")
    if len(set(cids)) != len(cids): raise PackageError("duplicate VST3 class ID")
    if set(cids) != set(identity["class_ids"]): raise PackageError("VST3 class IDs do not match the protected release identity")
    for item in classes:
        if item.get("Name") != identity["product_name"]: raise PackageError("VST3 class name does not match release identity")
        if item.get("Vendor") != identity["vendor"]: raise PackageError("VST3 class vendor does not match release identity")
        if item.get("Version") != identity["version"]: raise PackageError("VST3 class version does not match release identity")
    platform_dir = bundle / "Contents" / "x86_64-win"
    if not platform_dir.is_dir(): raise PackageError("VST3 bundle is missing Contents/x86_64-win")
    modules = [p for p in platform_dir.rglob("*") if p.is_file() and p.suffix.lower() == ".vst3"]
    if len(modules) != 1: raise PackageError(f"expected exactly one x86_64-win VST3 module, found {len(modules)}")
    all_modules = [p for p in bundle.rglob("*") if p.is_file() and p.suffix.lower() == ".vst3"]
    if set(all_modules) != set(modules): raise PackageError("unexpected VST3 modules outside Contents/x86_64-win")
    forbidden = [p for p in bundle.rglob("*") if p.is_file() and p.suffix.lower() in {".exe", ".component", ".app"}]
    if forbidden: raise PackageError("non-VST3 product files found in bundle: " + ", ".join(str(p) for p in forbidden))
    pe = inspect_pe_x64(modules[0])
    if original != normalized: moduleinfo.write_bytes(normalized)
    return {"moduleinfo_path": str(moduleinfo.relative_to(bundle)).replace("\\", "/"), "moduleinfo_original_sha256": sha256_bytes(original), "moduleinfo_normalized_sha256": sha256_bytes(normalized), "classes": [{"category": x["Category"], "cid": x["CID"]} for x in classes], "binary": {"path": str(modules[0].relative_to(bundle)).replace("\\", "/"), **pe}, "metadata_normalized": original != normalized}

def verify_archive(path: Path, identity: dict[str, Any]) -> None:
    with zipfile.ZipFile(path, "r") as archive:
        bad = archive.testzip()
        if bad: raise PackageError(f"ZIP CRC failure: {bad}")
        names = archive.namelist()
        if len(names) != len(set(names)): raise PackageError("duplicate ZIP entry names")
        if any(n.lower().endswith((".zip", ".exe", ".component", ".app")) for n in names): raise PackageError("archive contains a nested ZIP or non-VST3 product")
        bundles = {n.split("/", 1)[0] for n in names if ".vst3/" in n}
        if bundles != {"SkellyTune.vst3"}: raise PackageError(f"archive must contain exactly one SkellyTune.vst3 bundle, got {sorted(bundles)}")
        moduleinfos = [n for n in names if n.endswith("/Contents/Resources/moduleinfo.json")]
        if len(moduleinfos) != 1: raise PackageError("archive must contain exactly one moduleinfo.json")
        packaged_moduleinfo = archive.read(moduleinfos[0])
        packaged_text = packaged_moduleinfo.decode("utf-8-sig")
        if strip_trailing_commas(packaged_text) != packaged_text: raise PackageError("packaged moduleinfo.json still contains trailing commas")
        metadata, _ = load_strict_json(packaged_moduleinfo, "packaged moduleinfo.json")
        if metadata.get("Name") != identity["product_name"] or metadata.get("Version") != identity["version"]: raise PackageError("packaged moduleinfo identity changed during archive creation")
        pe_paths = [n for n in names if n.endswith("/Contents/x86_64-win/SkellyTune.vst3")]
        if len(pe_paths) != 1: raise PackageError("archive must contain exactly one Windows VST3 module")
        with tempfile.TemporaryDirectory(prefix="skellytune-pe-check-") as tmp:
            pe = Path(tmp) / "module.vst3"
            pe.write_bytes(archive.read(pe_paths[0]))
            inspect_pe_x64(pe)
        required = {"PREVIEW_ONLY.txt", "provenance.json", "SHA256SUMS.txt", "release/identity.json"}
        if not required.issubset(set(names)): raise PackageError("archive is missing preview warning, provenance, identity or checksum manifest")
        checksums = archive.read("SHA256SUMS.txt").decode("utf-8").splitlines()
        checked = set()
        for line in checksums:
            digest, separator, name = line.partition("  ")
            if not separator or len(digest) != 64 or name not in names: raise PackageError("malformed or unknown SHA256SUMS entry")
            if sha256_bytes(archive.read(name)) != digest: raise PackageError(f"SHA256SUMS mismatch: {name}")
            checked.add(name)
        if checked != (set(names) - {"SHA256SUMS.txt"}): raise PackageError("SHA256SUMS does not cover every packaged file exactly once")

def package_preview(args: argparse.Namespace) -> None:
    identity = load_identity(args.identity)
    validate_cmake_identity(args.cmake, identity)
    bundle, output = args.bundle.resolve(), args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="skellytune-package-") as tmp:
        stage = Path(tmp)
        staged_bundle = stage / bundle.name
        shutil.copytree(bundle, staged_bundle)
        report = validate_bundle(staged_bundle, identity)
        (stage / "PREVIEW_ONLY.txt").write_text(
            f"{identity['product_name']} {identity['version']} Windows x64 VST3 engineering preview.\n"
            "Not approved for sale or public redistribution.\n"
            "Manual test install: close the DAW, preserve the existing bundle, copy SkellyTune.vst3 to the user's VST3 folder, then rescan.\n"
            "If host validation fails, remove this preview and restore the preserved bundle.\n"
            "Publisher identity, support contact, EULA, third-party notices and commercial licensing remain pending owner approval.\n",
            encoding="utf-8",
        )
        (stage / "release").mkdir()
        shutil.copy2(args.identity, stage / "release" / "identity.json")
        provenance = {"schema_version": 1, "product": identity["product_name"], "version": identity["version"], "publisher_metadata": identity["vendor"], "manufacturer_code": identity["manufacturer_code"], "plugin_code": identity["plugin_code"], "format": "VST3", "platform": "Windows x64", "source": {"repository": args.repository, "ref": args.ref, "commit": args.source_sha, "workflow": args.workflow, "run_id": args.run_id}, "built_utc": datetime.now(timezone.utc).isoformat(), "bundle_validation": report, "commercial_release_approved": identity["owner_approved"] is True}
        (stage / "provenance.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        lines = [f"{sha256_file(p)}  {p.relative_to(stage).as_posix()}" for p in sorted(x for x in stage.rglob("*") if x.is_file())]
        (stage / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        if output.exists(): output.unlink()
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(p for p in stage.rglob("*") if p.is_file()): archive.write(path, path.relative_to(stage).as_posix())
    verify_archive(output, identity)
    digest = sha256_file(output)
    output.with_suffix(output.suffix + ".sha256").write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(f"PACKAGED {output} ({output.stat().st_size} bytes)")
    print(f"SHA256 {digest}")
    print("Strict moduleinfo JSON, AMD64 PE32+, exact three-class inventory and flat VST3-only ZIP verified.")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--check-release-approval", action="store_true")
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--cmake", type=Path, default=Path("CMakeLists.txt"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--source-sha", default=os.environ.get("GITHUB_SHA", "local"))
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", "local"))
    parser.add_argument("--workflow", default=os.environ.get("GITHUB_WORKFLOW", "local"))
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF", "local"))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", "local"))
    parser.add_argument("--preview-readme", type=Path)
    args = parser.parse_args()
    try:
        identity = load_identity(args.identity)
        if args.check_release_approval:
            check_release_approval(identity)
            print("Owner-approved public release identity and legal/support gates verified.")
            return 0
        if not args.bundle or not args.output: parser.error("--bundle and --output are required unless --check-release-approval is used")
        package_preview(args)
        return 0
    except (OSError, PackageError, zipfile.BadZipFile) as exc:
        print(f"RELEASE PACKAGE ERROR: {exc}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
