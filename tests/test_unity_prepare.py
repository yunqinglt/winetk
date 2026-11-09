#!/usr/bin/env python3
"""Check isolated resource import, refresh and interrupted-copy recovery."""
import importlib.util
from pathlib import Path
import pwd
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("prepare_unity", Path(__file__).resolve().parents[1]/"scripts/prepare-unity.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class CacheImport(unittest.TestCase):
    def test_isolated_cache_refresh(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steam, prefix = root/"steam", root/"prefix"
            original = steam/"steamapps/compatdata/1973530/pfx/drive_c/users/steamuser/AppData/LocalLow"
            local = prefix/"drive_c/users"/pwd.getpwuid(os.getuid()).pw_name/"AppData/LocalLow"
            bundle = Path("Unity/ProjectMoon_LimbusCompany/hash/version/__data")
            catalog = Path("ProjectMoon/LimbusCompany/com.unity.addressables/catalog.bin")
            save = Path("ProjectMoon/LimbusCompany/save_slot_test.json")
            for relative, data in ((bundle, b"asset bundle"), (catalog, b"old catalog"), (save, b"account save")):
                (original/relative).parent.mkdir(parents=True, exist_ok=True)
                (original/relative).write_bytes(data)
            (local/catalog).parent.mkdir(parents=True)
            (local/catalog).write_bytes(b"new project catalog")
            prepare.import_limbus_cache(steam, prefix)
            self.assertEqual((local/bundle).read_bytes(), b"asset bundle")
            self.assertEqual((local/catalog).read_bytes(), b"new project catalog")
            self.assertFalse((local/save).exists())
            # Destination writes cannot affect Steam's live cache.
            (local/bundle).write_bytes(b"project updated")
            self.assertEqual((original/bundle).read_bytes(), b"asset bundle")
            extra = original/bundle.parent/"__info"
            extra.write_bytes(b"metadata")
            prepare.import_limbus_cache(steam, prefix)
            self.assertFalse((local/bundle.parent/"__info").exists())
            prepare.import_limbus_cache(steam, prefix, refresh=True)
            self.assertEqual((local/bundle.parent/"__info").read_bytes(), b"metadata")
            self.assertEqual((local/bundle).read_bytes(), b"project updated")

    def test_interrupted_copy_is_not_published(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, destination = root/"source", root/"destination"
            source.mkdir()
            (source/"bundle").write_bytes(b"complete")

            def fail_copy(argv, **kwargs):
                staged = Path(argv[-1])
                staged.mkdir()
                (staged/"bundle").write_bytes(b"partial")
                raise subprocess.CalledProcessError(1, argv)

            with patch.object(prepare.subprocess, "run", side_effect=fail_copy):
                with self.assertRaises(subprocess.CalledProcessError):
                    prepare.copy_missing(source, destination)
            self.assertFalse(destination.exists())
            self.assertFalse(list(root.glob(".winetk-cache-*")))
            prepare.copy_missing(source, destination)
            self.assertEqual((destination/"bundle").read_bytes(), b"complete")


if __name__ == "__main__":
    unittest.main()
