#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Execute stock/candidate ext4 orphan initialization against faulting reads."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from evonix_final_audit_test import function, source

MOCK = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
typedef uint32_t __le32;
#ifndef EFSCORRUPTED
#define EFSCORRUPTED 117
#endif
#define KERN_ERR ""
#define EXT4_IGET_SPECIAL 0
#define GFP_KERNEL 0
#define EXT4_MAX_ORPHAN_FILE_BLOCKS 1000ULL
#define EXT4_ORPHAN_BLOCK_MAGIC 0x1234
#define le32_to_cpu(x) (x)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
struct atomic { int count; };
static void atomic_set(struct atomic *a,int v) { a->count=v; }
struct buffer_head { __le32 data[4]; void *b_data; };
struct ext4_orphan_block { struct buffer_head *ob_bh; struct atomic ob_free_entries; };
struct ext4_orphan_info { int of_blocks, of_csum_seed; struct ext4_orphan_block *of_binfo; };
struct ext4_orphan_block_tail { __le32 ob_magic; };
struct es { __le32 s_orphan_file_inum; };
struct sbi { struct ext4_orphan_info s_orphan_info; struct es *s_es; };
struct super_block { struct sbi *sbi; unsigned s_blocksize_bits; };
struct inode { unsigned long long i_size; unsigned i_blkbits; int i_csum_seed; };
#define EXT4_SB(sb) ((sb)->sbi)
#define EXT4_I(inode) (inode)
static struct inode ino;
static struct buffer_head blocks[8];
static struct ext4_orphan_block_tail tail;
static int fail_type, fail_index, bh_refs, inode_refs, allocated, current_index;
static bool ext4_has_feature_orphan_file(struct super_block *sb) { (void)sb; return true; }
static int ext4_inodes_per_orphan_block(struct super_block *sb) { (void)sb; return 4; }
static struct inode *ext4_iget(struct super_block *sb, ino_t n, int f)
{ (void)sb; (void)n; (void)f; inode_refs++; return &ino; }
static void iput(struct inode *i) { (void)i; inode_refs--; }
static void *kvmalloc_array(size_t n, size_t size, int flags)
{ (void)flags; void *p=calloc(n,size); if(p) allocated++; return p; }
static void kvfree(void *p) { free(p); allocated--; }
static struct buffer_head *ext4_bread(void *handle, struct inode *i, int index, int flags)
{ (void)handle; (void)i; (void)flags; current_index=index;
  if(index==fail_index && fail_type==1) return (void *)(intptr_t)-EIO;
  if(index==fail_index && fail_type==2) return NULL;
  bh_refs++; return &blocks[index]; }
static void brelse(struct buffer_head *bh) { (void)bh; bh_refs--; }
static struct ext4_orphan_block_tail *ext4_orphan_block_tail(struct super_block *sb,
                                                          struct buffer_head *bh)
{ (void)sb; (void)bh;
  tail.ob_magic = current_index==fail_index && fail_type==3 ? 0 : EXT4_ORPHAN_BLOCK_MAGIC;
  return &tail; }
static bool ext4_orphan_file_block_csum_verify(struct super_block *sb,struct buffer_head *bh)
{ (void)sb; (void)bh; return !(current_index==fail_index && fail_type==4); }
#define ext4_msg(...) ((void)0)
#define ext4_error(...) ((void)0)
"""

MAIN = r"""
int main(void) {
    struct es es={.s_orphan_file_inum=3};
    struct sbi sbi={.s_es=&es};
    struct super_block sb={.sbi=&sbi,.s_blocksize_bits=12};
    ino.i_size=8*4096; ino.i_blkbits=12;
    for(int i=0;i<8;i++) blocks[i].b_data=blocks[i].data;
    int version, kind, index;
    while(scanf("%d %d %d",&version,&kind,&index)==3) {
        bh_refs=inode_refs=allocated=0; fail_type=kind; fail_index=index;
        int result=version ? candidate(&sb) : baseline(&sb);
        printf("%d %d %d %d\n",result,bh_refs,inode_refs,allocated);
    }
    return 0;
}
"""


class OrphanCleanupC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-orphan-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        c = MOCK
        for name, baseline in (("baseline", True), ("candidate", False)):
            c += f"static int {name}(struct super_block *sb) {{\n"
            c += function(source("fs/ext4/orphan.c", baseline),
                          "ext4_init_orphan_info") + "\n}\n"
        (directory / "orphan.c").write_text(c + MAIN)
        cls.exe = directory / "orphan"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined,bounds", "-fno-sanitize-recover=all",
                        str(directory / "orphan.c"), "-o", str(cls.exe)], check=True)

    def evaluate(self, cases):
        data = "".join(" ".join(map(str,c))+"\n" for c in cases)
        out = subprocess.check_output([str(self.exe)], input=data, text=True)
        return [tuple(map(int,line.split())) for line in out.splitlines()]

    def test_bad_magic_releases_the_current_and_all_previous_buffers(self):
        for i in range(8):
            self.assertEqual(self.evaluate([(0,3,i),(1,3,i)]),
                             [(-5,1,0,0),(-5,0,0,0)])

    def test_bad_checksum_releases_the_current_and_all_previous_buffers(self):
        for i in range(8):
            self.assertEqual(self.evaluate([(0,4,i),(1,4,i)]),
                             [(-5,1,0,0),(-5,0,0,0)])

    def test_failed_read_is_not_mistaken_for_an_owned_buffer(self):
        cases = [(v,kind,i) for v in (0,1) for kind in (1,2) for i in range(8)]
        self.assertEqual(self.evaluate(cases), [(-5,0,0,0)] * len(cases))


if __name__ == "__main__":
    unittest.main(verbosity=2)
