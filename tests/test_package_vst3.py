import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts.package_vst3 import PackageError, check_release_approval, load_strict_json, package_preview, strip_trailing_commas, validate_bundle

IDENTITY = {
    "schema_version": 1, "product_name": "SkellyTune", "version": "1.1.2", "vendor": "HumHouse",
    "manufacturer_code": "Humh", "plugin_code": "Sktn",
    "class_ids": ["ABCDEF019182FAEB48756D68536B746E", "ABCDEF011234ABCD48756D68536B746E", "ABCDEF01C0DEF00D48756D68536B746E"],
    "owner_approved": False, "vendor_url": "", "support_email": "", "license_status": "pending",
    "third_party_notices_status": "missing", "eula_status": "missing", "trademark_status": "pending",
}

def fake_pe(machine=0x8664, optional_magic=0x20B):
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    struct.pack_into("<H", data, 0x98, optional_magic)
    return bytes(data)

def moduleinfo(duplicate=False):
    ids = list(IDENTITY["class_ids"])
    if duplicate: ids[2] = ids[0]
    categories = ["Audio Module Class", "Component Controller Class", "Plugin Compatibility Class"]
    classes = [{"CID": cid, "Category": category, "Name": IDENTITY["product_name"], "Vendor": IDENTITY["vendor"], "Version": IDENTITY["version"]} for cid, category in zip(ids, categories)]
    value = {"Name": IDENTITY["product_name"], "Version": IDENTITY["version"], "Factory Info": {"Vendor": IDENTITY["vendor"], "URL": "", "E-Mail": ""}, "Classes": classes}
    return (json.dumps(value, indent=2)[:-2] + ",\n}\n").encode()

def make_bundle(root, machine=0x8664, optional_magic=0x20B, duplicate=False):
    bundle = root / "SkellyTune.vst3"
    resources = bundle / "Contents" / "Resources"
    binary_dir = bundle / "Contents" / "x86_64-win"
    resources.mkdir(parents=True)
    binary_dir.mkdir(parents=True)
    (resources / "moduleinfo.json").write_bytes(moduleinfo(duplicate))
    (binary_dir / "SkellyTune.vst3").write_bytes(fake_pe(machine, optional_magic))
    return bundle

def cmake_file(root):
    path = root / "CMakeLists.txt"
    path.write_text('''project(HumHouseVocalTune VERSION 1.1.2 LANGUAGES C CXX)\njuce_add_plugin(HumHouseVocalTune\n COMPANY_NAME "HumHouse"\n PLUGIN_MANUFACTURER_CODE Humh\n PLUGIN_CODE Sktn\n PRODUCT_NAME "SkellyTune")\n''', encoding="utf-8")
    return path

class PackageTests(unittest.TestCase):
    def test_trailing_comma_stripper_preserves_strings(self):
        self.assertEqual(strip_trailing_commas('{"text":"comma, } inside string", "list":[1,2,],}'), '{"text":"comma, } inside string", "list":[1,2]}')

    def test_rejects_duplicate_json_keys(self):
        with self.assertRaisesRegex(PackageError, "duplicate JSON key"):
            load_strict_json(b'{"Name":"one","Name":"two"}', "duplicate fixture")

    def test_rejects_nonfinite_json_numbers(self):
        with self.assertRaisesRegex(PackageError, "non-finite JSON value"):
            load_strict_json(b'{"peak":NaN}', "nonfinite fixture")

    def test_valid_bundle_normalizes_metadata_and_checks_x64(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = make_bundle(Path(tmp))
            report = validate_bundle(bundle, IDENTITY)
            self.assertTrue(report["metadata_normalized"])
            self.assertEqual(json.loads((bundle / "Contents/Resources/moduleinfo.json").read_text())["Name"], "SkellyTune")
            self.assertEqual(report["binary"]["machine"], "AMD64")

    def test_rejects_wrong_pe_architecture(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(PackageError, r"AMD64 PE32\+"):
                validate_bundle(make_bundle(Path(tmp), 0x14C, 0x10B), IDENTITY)

    def test_rejects_duplicate_class_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(PackageError, "duplicate VST3 class ID"):
                validate_bundle(make_bundle(Path(tmp), duplicate=True), IDENTITY)

    def test_rejects_wrong_class_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = make_bundle(Path(tmp))
            path = bundle / "Contents/Resources/moduleinfo.json"
            data = json.loads(strip_trailing_commas(path.read_text()))
            data["Classes"].pop()
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(PackageError, "exactly three"):
                validate_bundle(bundle, IDENTITY)

    def test_rejects_vendor_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = make_bundle(Path(tmp))
            path = bundle / "Contents/Resources/moduleinfo.json"
            data = json.loads(strip_trailing_commas(path.read_text()))
            data["Factory Info"]["Vendor"] = "Other"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(PackageError, "Factory Info Vendor"):
                validate_bundle(bundle, IDENTITY)

    def test_release_gate_fails_until_owner_approves(self):
        with self.assertRaisesRegex(PackageError, "public release blocked"):
            check_release_approval(IDENTITY)
        approved = dict(IDENTITY, owner_approved=True, vendor_url="https://vendor.example", support_email="support@example.invalid", license_status="approved", third_party_notices_status="complete", eula_status="approved", trademark_status="cleared")
        check_release_approval(approved)

    def test_package_contains_flat_vst3_provenance_and_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bundle = make_bundle(root)
            identity = root / "identity.json"
            identity.write_text(json.dumps(IDENTITY), encoding="utf-8")
            output = root / "artifacts" / "SkellyTune-Windows-x64.zip"
            class Args: pass
            args = Args()
            args.identity, args.cmake, args.bundle, args.output = identity, cmake_file(root), bundle, output
            args.repository, args.ref, args.source_sha, args.run_id, args.workflow = "skellyruno/skell-autotune-vst", "refs/heads/test", "a" * 40, "123", "test"
            args.preview_readme = None
            package_preview(args)
            with zipfile.ZipFile(output) as archive:
                names = archive.namelist()
                self.assertIn("PREVIEW_ONLY.txt", names)
                self.assertIn("provenance.json", names)
                self.assertIn("SHA256SUMS.txt", names)
                self.assertFalse(any(n.lower().endswith((".exe", ".zip", ".component")) for n in names))
                self.assertEqual(sum(n.endswith("moduleinfo.json") for n in names), 1)
            self.assertTrue(output.with_suffix(output.suffix + ".sha256").is_file())

if __name__ == "__main__": unittest.main()
