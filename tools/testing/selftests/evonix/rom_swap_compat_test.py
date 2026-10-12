#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Source/configuration contracts only; not a boot or ABI test."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]


class RomSwapCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modules = {}
        exec(compile((ROOT / "modules.bzl").read_text(), "modules.bzl", "exec"),
             cls.modules)
        cls.config = (ROOT / "arch/arm64/configs/evonix.config").read_text()

    def test_arm64_keeps_rom_compatible_module_outputs(self):
        modules = self.modules["get_gki_modules_list"]("arm64")
        self.assertIn("drivers/block/zram/zram.ko", modules)
        self.assertIn("mm/zsmalloc.ko", modules)
        self.assertEqual(len(modules), len(set(modules)))

    def test_other_architectures_keep_modular_swap_drivers(self):
        for arch in ("arm", "i386", "x86_64"):
            modules = self.modules["get_gki_modules_list"](arch)
            self.assertIn("drivers/block/zram/zram.ko", modules)
            self.assertIn("mm/zsmalloc.ko", modules)

    def test_other_common_modules_are_preserved(self):
        expected = set(self.modules["_COMMON_GKI_MODULES_LIST"])
        expected.update(self.modules["_ARM64_GKI_MODULES_LIST"])
        self.assertEqual(expected,
                         set(self.modules["get_gki_modules_list"]("arm64")))

    def test_arm64_base_and_evonix_keep_modular_swap(self):
        base = (ROOT / "arch/arm64/configs/gki_defconfig").read_text()
        self.assertIn("\nCONFIG_ZRAM=m\n", base)
        for option in ("ZRAM", "ZSMALLOC"):
            self.assertIn(f"\nCONFIG_{option}=m\n", self.config)
            self.assertNotIn(f"\nCONFIG_{option}=y\n", self.config)

    def test_compressors_remain_available_at_boot(self):
        base = (ROOT / "arch/arm64/configs/gki_defconfig").read_text()
        for option in ("CRYPTO_LZO", "CRYPTO_LZ4", "CRYPTO_ZSTD"):
            self.assertIn(f"\nCONFIG_{option}=y\n", base)

    def test_o2_is_explicit_and_keeps_thinlto(self):
        self.assertIn("\nCONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y\n", self.config)
        self.assertNotIn("\nCONFIG_CC_OPTIMIZE_FOR_PERFORMANCE_O3=y\n", self.config)
        self.assertIn("\nCONFIG_LTO_CLANG_THIN=y\n", self.config)
        script = (ROOT / "build-evonix.sh").read_text()
        for option in ("CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y",
                       "CONFIG_ZRAM=m", "CONFIG_ZSMALLOC=m"):
            self.assertIn(option, script)
        self.assertIn("PERFORMANCE_O3|SIZE", script)

    def test_verifier_reads_image_and_rejects_builtin_swap(self):
        script = (ROOT / "build-evonix.sh").read_text()
        self.assertIn('CFG="$DIST/embedded.config"', script)
        self.assertIn('"$COMMON_DIR/scripts/extract-ikconfig" "$DIST/Image"', script)
        self.assertIn('"$DIST/modules.builtin"', script)
        self.assertIn("(^|/)(zram|zsmalloc)", script)
        self.assertNotIn("-printf '%T@ %p", script)


if __name__ == "__main__":
    unittest.main()
