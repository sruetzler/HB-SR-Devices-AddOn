"""Exercise package assembly with one and multiple device source trees."""

import importlib.util
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_addon", ROOT / "scripts/build_addon.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class AddonBuildTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hb-addon-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copytree(ROOT / "addon", self.root / "addon")
        shutil.copytree(ROOT / "devices/HB-SR-HY/ccu", self.root / "devices/HB-SR-HY/ccu")

    def second_device(self, filename="hb-sr-test.xml", model="HB-SR-TEST"):
        ccu = self.root / "devices/HB-SR-TEST/ccu"
        ccu.mkdir(parents=True)
        (ccu / filename).write_text(
            f'<device><supported_types><type id="{model}"/></supported_types></device>'
        )
        return ccu

    def test_current_xml_scripts_assets_and_update_urls(self):
        result = builder.build(self.root)
        expected = self.root / "devices/HB-SR-HY/ccu/hb-sr-hy.xml"
        with tarfile.open(result) as archive:
            self.assertEqual(archive.extractfile("addon/firmware/rftypes/hb-sr-hy.xml").read(), expected.read_bytes())
            self.assertEqual(archive.extractfile("addon/VERSION").read(), (self.root / "addon/VERSION").read_bytes())
            for name in ("update_script", "rc.d/hb-sr-devices-addon", "addon/install_hb-sr-hy", "addon/uninstall_hb-sr-hy", "addon/update-check.cgi"):
                self.assertEqual(archive.getmember(name).mode, 0o755)
            self.assertEqual(archive.getmember("addon/firmware/rftypes/hb-sr-hy.xml").mode, 0o644)
            self.assertIn("addon/www/config/img/devices/50/hb-sr-hy_thumb.png", archive.getnames())
            self.assertTrue(all(not name.startswith("/") and ".." not in Path(name).parts for name in archive.getnames()))
            update = archive.extractfile("addon/update-check.cgi").read().decode()
            self.assertIn("/repos/sruetzler/HB-SR-Devices-AddOn/releases/latest", update)
            self.assertIn("hb-sr-devices-addon.tgz", update)
        self.assertEqual(result.read_bytes(), (self.root / "CCU_RM" / builder.PACKAGE).read_bytes())
        self.assertEqual((self.root / "addon/VERSION").read_bytes(), (self.root / "CCU_RM/src/addon/VERSION").read_bytes())

    def test_second_device_is_included_automatically(self):
        ccu = self.second_device()
        (ccu / "install_test").write_text("#!/bin/sh\nexit 0\n")
        with tarfile.open(builder.build(self.root)) as archive:
            self.assertIn("addon/firmware/rftypes/hb-sr-test.xml", archive.getnames())
            self.assertIn("addon/firmware/rftypes/hb-sr-hy.xml", archive.getnames())
            self.assertEqual(archive.getmember("addon/install_test").mode, 0o755)

    def test_build_is_reproducible(self):
        first = builder.build(self.root).read_bytes()
        self.assertEqual(first, builder.build(self.root).read_bytes())

    def test_duplicate_filename_does_not_replace_previous_package(self):
        previous = builder.build(self.root).read_bytes()
        self.second_device(filename="hb-sr-hy.xml")
        with self.assertRaisesRegex(ValueError, "Duplicate package path"):
            builder.build(self.root)
        self.assertEqual(previous, (self.root / "dist" / builder.PACKAGE).read_bytes())
        self.assertEqual(previous, (self.root / "CCU_RM" / builder.PACKAGE).read_bytes())

    def test_duplicate_device_type_is_rejected(self):
        self.second_device(model="HB-SR-HY")
        with self.assertRaisesRegex(ValueError, "Duplicate device type"):
            builder.build(self.root)

    def test_invalid_xml_is_rejected(self):
        (self.root / "devices/HB-SR-HY/ccu/hb-sr-hy.xml").write_text("<device>")
        with self.assertRaises(ET.ParseError):
            builder.build(self.root)


if __name__ == "__main__":
    unittest.main()
