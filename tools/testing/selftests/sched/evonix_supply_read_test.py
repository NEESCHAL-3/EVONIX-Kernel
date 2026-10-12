#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the actual supply helper with mocked lifetimes and callbacks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]

MOCK = r"""
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <sys/types.h>
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define KERNFS_FILE 1
struct kernfs_node { void *priv; struct kernfs_node *parent; unsigned mode; };
struct kobject;
struct sysfs_ops {
    ssize_t (*show)(struct kobject *, void *, char *);
    ssize_t (*store)(struct kobject *, void *, const char *, size_t);
};
struct kobj_type { const struct sysfs_ops *sysfs_ops; };
struct kobject { struct kernfs_node *sd; struct kobj_type *ktype; };
struct power_supply { struct { struct kobject kobj; } dev; };
static struct power_supply supply;
static struct kernfs_node parent, node;
static struct kobj_type type;
static struct sysfs_ops ops;
static int fault, refs, active, allocations, stores;
static int sysfs_symlink_target_lock;
static ssize_t show_length, store_length;
static const char *payload;
static const char *written;
static struct power_supply *power_supply_get_by_name(const char *s)
{ (void)s; if (fault==1) return NULL; refs++; return &supply; }
static void power_supply_put(struct power_supply *s)
{ (void)s; refs--; }
static void spin_lock(int *l) { (void)l; }
static void spin_unlock(int *l) { (void)l; }
static void kernfs_get(struct kernfs_node *k) { (void)k; refs++; }
static void kernfs_put(struct kernfs_node *k) { (void)k; refs--; }
static struct kernfs_node *sysfs_get_dirent(struct kernfs_node *k, const char *n)
{ (void)k; (void)n; if (fault==3) return NULL; refs++; return &node; }
static bool kernfs_get_active(struct kernfs_node *k)
{ (void)k; if (fault==4) return false; active++; return true; }
static void kernfs_put_active(struct kernfs_node *k) { (void)k; active--; }
static int kernfs_type(struct kernfs_node *k)
{ (void)k; return fault==5 ? 0 : KERNFS_FILE; }
static void *kmalloc(size_t size, int flags)
{ (void)flags; assert(size==PAGE_SIZE); if (fault==6) return NULL;
  void *p=malloc(size); assert(p); memset(p, 0xa5, size); allocations++; return p; }
static void kfree(void *p) { free(p); allocations--; }
static int kstrtoint(const char *s, unsigned base, int *out)
{
    char *end;
    errno=0;
    long v=strtol(s, &end, base);
    if (errno==ERANGE || v<INT_MIN || v>INT_MAX) return -ERANGE;
    if (end==s) return -EINVAL;
    if (*end=='\n') end++;
    if (*end) return -EINVAL;
    *out=(int)v;
    return 0;
}
static ssize_t show(struct kobject *k, void *attr, char *buf)
{ (void)k; (void)attr; if (show_length>0 && show_length<PAGE_SIZE)
  { memset(buf,'0',show_length); if(payload) memcpy(buf,payload,strlen(payload)); }
  return show_length; }
static ssize_t store(struct kobject *k, void *attr, const char *buf, size_t size)
{ (void)k; (void)attr; assert(size==2); stores++; written=buf; return store_length; }
"""

MAIN = r"""
int main(int argc, char **argv) {
    assert(argc==2);
    int test=atoi(argv[1]), value=77, ret=0, expected=0;
    ops.show=show; ops.store=store; type.sysfs_ops=&ops;
    supply.dev.kobj.sd=&parent; supply.dev.kobj.ktype=&type;
    parent.priv=&supply.dev.kobj; node.parent=&parent; node.priv=&node; node.mode=0664;
    payload="123\n"; show_length=4; store_length=2;
    switch(test) {
    case 0: break;
    case 1: payload="123"; show_length=3; break;
    case 2: payload="-42\n"; show_length=4; break;
    case 3: payload=NULL; show_length=PAGE_SIZE-1; break;
    case 4: show_length=PAGE_SIZE; expected=-EOVERFLOW; break;
    case 5: show_length=PAGE_SIZE+1; expected=-EOVERFLOW; break;
    case 6: show_length=0; expected=-EIO; break;
    case 7: show_length=-ENODEV; expected=-ENODEV; break;
    case 8: payload="123x"; expected=-EINVAL; break;
    case 9: payload="2147483648"; show_length=10; expected=-ERANGE; break;
    case 10: fault=1; expected=-ENODEV; break;
    case 11: supply.dev.kobj.sd=NULL; expected=-ENODEV; break;
    case 12: fault=3; expected=-ENOENT; break;
    case 13: fault=4; expected=-ENODEV; break;
    case 14: fault=5; expected=-EOPNOTSUPP; break;
    case 15: fault=6; expected=-ENOMEM; break;
    case 16: ops.show=NULL; expected=-EOPNOTSUPP; break;
    case 17: node.mode=0222; expected=-EOPNOTSUPP; break;
    case 18: parent.priv=NULL; expected=-EOPNOTSUPP; break;
    case 19: type.sysfs_ops=NULL; expected=-EOPNOTSUPP; break;
    case 20: ret=evonix_oem_navigation_set(true);
             assert(stores==1 && !strcmp(written,"3\n")); goto finish;
    case 21: ret=evonix_oem_navigation_set(false);
             assert(stores==1 && !strcmp(written,"2\n")); goto finish;
    case 22: store_length=1; expected=-EIO;
             ret=evonix_oem_navigation_set(true); goto finish;
    case 23: ops.store=NULL; expected=-EOPNOTSUPP;
             ret=evonix_oem_navigation_set(true); goto finish;
    case 24: expected=-EINVAL;
             ret=evonix_oem_supply_read("battery","voltage_now",&value); goto finish;
    case 25: expected=-EINVAL;
             ret=evonix_oem_supply_read("usb","arbitrary",&value); goto finish;
    default: assert(false);
    }
    ret=evonix_oem_supply_read("usb","online",&value);
    if(test==0 || test==1) assert(value==123);
    if(test==2) assert(value==-42);
    if(test==3) assert(value==0);
    if(expected) assert(value==77);
finish:
    assert(ret==expected);
    assert(refs==0 && active==0 && allocations==0);
    return 0;
}
"""


class SupplyReadC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="evonix-supply-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        helper = (ROOT / "fs/sysfs/evonix_supply.c").read_text()
        helper = helper[helper.index("static int evx_attribute"):]
        (directory / "supply.c").write_text(MOCK + helper + MAIN)
        cls.exe = directory / "supply"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined,bounds", "-fno-sanitize-recover=all",
                        str(directory / "supply.c"), "-o", str(cls.exe)], check=True)

    def test_callback_bounds_and_lifetime_cleanup(self):
        for scenario in range(26):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.exe), str(scenario)], check=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
