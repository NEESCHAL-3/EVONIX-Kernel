// SPDX-License-Identifier: GPL-2.0
/* EVONIX / NEESCHAL: lifetime-safe access to existing OEM charge policy.
 * Only navigation pause can be written. No filesystem paths, credentials,
 * private vendor layouts, cached callback pointers, or dynamic code patches.
 */
#include <linux/evonix_oem_bypass.h>
#include <linux/kobject.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include "sysfs.h"
#include "../kernfs/kernfs-internal.h"

static int evx_attribute(const char *supply, const char *name,
	bool write, int *value)
{
	struct power_supply *psy;
	struct kernfs_node *kn, *parent = NULL;
	struct kobject *kobj;
	const struct sysfs_ops *ops;
	char *buf;
	ssize_t len;
	int ret;

	psy = power_supply_get_by_name(supply);
	if (!psy)
		return -ENODEV;
	kobj = &psy->dev.kobj;
	/* device_del() may remove sysfs while the supply reference still exists. */
	spin_lock(&sysfs_symlink_target_lock);
	if (kobj->sd) {
		parent = kobj->sd;
		kernfs_get(parent);
	}
	spin_unlock(&sysfs_symlink_target_lock);
	if (!parent) {
		ret = -ENODEV;
		goto put_supply;
	}
	kn = sysfs_get_dirent(parent, name);
	kernfs_put(parent);
	if (!kn) {
		ret = -ENOENT;
		goto put_supply;
	}
	if (!kernfs_get_active(kn)) {
		ret = -ENODEV;
		goto put_node;
	}
	ret = -EOPNOTSUPP;
	if (kernfs_type(kn) != KERNFS_FILE || kn->parent->priv != kobj ||
	    !kn->priv || !kobj->ktype || !kobj->ktype->sysfs_ops)
		goto put_active;
	ops = kobj->ktype->sysfs_ops;
	if (write) {
		if (!ops->store || !(kn->mode & 0222))
			goto put_active;
		/* No caller-supplied commands: only this stock navigation slot. */
		len = ops->store(kobj, kn->priv, *value ? "3\n" : "2\n", 2);
		ret = len == 2 ? 0 : len < 0 ? len : -EIO;
	} else {
		if (!ops->show || !(kn->mode & 0444))
			goto put_active;
		buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto put_active;
		}
		len = ops->show(kobj, kn->priv, buf);
		ret = len <= 0 ? (len ? len : -EIO) :
			len >= PAGE_SIZE ? -EOVERFLOW : kstrtoint(buf, 0, value);
		kfree(buf);
	}
put_active:
	kernfs_put_active(kn);
put_node:
	kernfs_put(kn);
put_supply:
	power_supply_put(psy);
	return ret;
}

int evonix_oem_supply_read(const char *supply, const char *name, int *value)
{
	/* Deliberately limited to the measurements used by the bypass backend. */
	if (!strcmp(supply, "battery") && !strcmp(name, "smart_chg"))
		return evx_attribute(supply, name, false, value);
	if (strcmp(supply, "usb"))
		return -EINVAL;
	if (strcmp(name, "cp_sm_run_state") && strcmp(name, "online") &&
	    strcmp(name, "input_suspend") && strcmp(name, "pmic_vbus"))
		return -EINVAL;
	return evx_attribute(supply, name, false, value);
}

int evonix_oem_navigation_set(bool enabled)
{
	int value = enabled;

	return evx_attribute("battery", "smart_chg", true, &value);
}
