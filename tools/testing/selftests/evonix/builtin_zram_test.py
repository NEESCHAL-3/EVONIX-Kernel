#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Source/configuration contracts only; not a boot or ABI test."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[4]


class BuiltinZramTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modules = {}
        exec(compile((ROOT / "modules.bzl").read_text(), "modules.bzl", "exec"),
             cls.modules)
        cls.config = (ROOT / "arch/arm64/configs/evonix.config").read_text()

    def test_arm64_does_not_declare_builtin_module_outputs(self):
        modules = self.modules["get_gki_modules_list"]("arm64")
        self.assertNotIn("drivers/block/zram/zram.ko", modules)
        self.assertNotIn("mm/zsmalloc.ko", modules)
        self.assertEqual(len(modules), len(set(modules)))

    def test_other_architectures_keep_modular_swap_drivers(self):
        for arch in ("arm", "i386", "x86_64"):
            modules = self.modules["get_gki_modules_list"](arch)
            self.assertIn("drivers/block/zram/zram.ko", modules)
            self.assertIn("mm/zsmalloc.ko", modules)

    def test_other_common_modules_are_preserved(self):
        expected = set(self.modules["_COMMON_GKI_MODULES_LIST"])
        expected -= {"drivers/block/zram/zram.ko", "mm/zsmalloc.ko"}
        expected.update(self.modules["_ARM64_GKI_MODULES_LIST"])
        self.assertEqual(expected,
                         set(self.modules["get_gki_modules_list"]("arm64")))

    def test_arm64_base_and_evonix_select_builtin_zram(self):
        base = (ROOT / "arch/arm64/configs/gki_defconfig").read_text()
        self.assertIn("\nCONFIG_ZRAM=y\n", base)
        for option in ("ZRAM", "ZSMALLOC", "ZRAM_WRITEBACK", "ZRAM_MULTI_COMP"):
            self.assertIn(f"\nCONFIG_{option}=y\n", self.config)

    def test_compressors_remain_available_at_boot(self):
        base = (ROOT / "arch/arm64/configs/gki_defconfig").read_text()
        for option in ("CRYPTO_LZO", "CRYPTO_LZ4", "CRYPTO_ZSTD"):
            self.assertIn(f"\nCONFIG_{option}=y\n", base)

    def test_o3_is_explicit_and_keeps_thinlto(self):
        self.assertIn("\nCONFIG_CC_OPTIMIZE_FOR_PERFORMANCE_O3=y\n", self.config)
        self.assertNotIn("\nCONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y\n", self.config)
        self.assertIn("\nCONFIG_LTO_CLANG_THIN=y\n", self.config)
        script = (ROOT / "build-evonix.sh").read_text()
        for option in ("CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE_O3=y",
                       "CONFIG_ZRAM=y", "CONFIG_ZSMALLOC=y"):
            self.assertIn(option, script)


if __name__ == "__main__":
    unittest.main()
