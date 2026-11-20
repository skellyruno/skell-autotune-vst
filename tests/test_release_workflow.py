from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")


class ReleaseWorkflowContractTests(unittest.TestCase):
    def test_pluginval_action_uses_a_published_matching_tag(self):
        refs = re.findall(
            r"(?m)^\s+uses:\s*Tracktion/pluginval/validate@([^\s#]+)\s*$",
            WORKFLOW,
        )
        self.assertEqual(refs, ["v2.0.0-beta3"])
        self.assertRegex(
            WORKFLOW,
            r"(?m)^\s+pluginval-version:\s*v2\.0\.0-beta3\s*$",
        )

    def test_windows_release_build_waits_for_offline_dsp_gate_before_publish(self):
        build = re.search(
            r"(?ms)^  build:\n(.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
            WORKFLOW,
        )
        self.assertIsNotNone(build)
        self.assertNotRegex(build.group(1), r"(?m)^\s+needs:")
        dsp = re.search(
            r"(?ms)^  dsp-regression:\n(.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
            WORKFLOW,
        )
        self.assertIsNotNone(dsp)
        for required in (
            "SKELLYTUNE_DSP_TESTS_ONLY=ON",
            "cmake --build build-dsp",
            "ctest --test-dir build-dsp",
        ):
            self.assertIn(required, dsp.group(1))
        publish = re.search(r"(?ms)^  publish:\n(.*)\Z", WORKFLOW)
        self.assertIsNotNone(publish)
        self.assertRegex(publish.group(1), r"(?m)^\s+needs:\s*\[build,\s*dsp-regression\]\s*$")

    def test_public_prs_withhold_product_artifacts(self):
        upload = re.search(
            r"(?ms)^      - name: Upload verified Windows VST3 preview\n(.*?)(?=^      - name:|^  publish:)",
            WORKFLOW,
        )
        self.assertIsNotNone(upload)
        self.assertRegex(
            upload.group(1),
            r"(?m)^        if:\s*github\.repository_visibility == 'private' \|\| github\.event_name == 'release'\s*$",
        )

    def test_superseded_cancellation_excludes_protected_refs(self):
        self.assertIn("github.event_name == 'pull_request'", WORKFLOW)
        for required in (
            "github.event.pull_request.head.ref != 'main'",
            "github.event.pull_request.head.ref != 'master'",
            "github.event.pull_request.head.ref != 'production'",
            "!startsWith(github.event.pull_request.head.ref, 'release/')",
            "github.event.pull_request.base.ref != 'production'",
            "!startsWith(github.event.pull_request.base.ref, 'release/')",
        ):
            self.assertIn(required, WORKFLOW)


if __name__ == "__main__":
    unittest.main()
