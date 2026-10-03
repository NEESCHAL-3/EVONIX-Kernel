// SPDX-License-Identifier: GPL-2.0
/* NEESCHAL / EVONIX: use Rodin's stock navigation charge-pause policy.
 * Image-only experimental backend. No private structure offsets, kprobes,
 * charge-pump mode writes, input suspension, or safety-policy overrides.
 * A neutral-current observation does NOT prove physical battery isolation.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/evonix_oem_bypass.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define EVX_SMART "battery/smart_chg"
#define EVX_USB "usb/"
#define EVX_NAV BIT(1)
static DEFINE_MUTEX(evx_command_lock);
static DEFINE_MUTEX(evx_state_lock);
static struct power_supply *evx_battery;
static bool evx_owned, evx_target;
static int evx_error, evx_attach_attempts;
static unsigned long evx_neutral_since, evx_last_sample;

/* The built-in accessor uses kernfs active references around OEM callbacks. */
static int evx_read_int(const char *path, int *value)
{
	if (!strcmp(path, EVX_SMART))
		return evonix_oem_supply_read("battery", "smart_chg", value);
	if (strncmp(path, EVX_USB, 4))
		return -EINVAL;
	return evonix_oem_supply_read("usb", path + 4, value);
}

static int evx_write_nav(bool enabled)
{
	int state, ret;

	/* Only navigation: enable bit + SOC threshold zero. Other OEM features
	 * and their parameters are left alone. Disable relinquishes this slot.
	 */
	ret = evonix_oem_navigation_set(enabled);
	if (ret)
		return ret;
	ret = evx_read_int(EVX_SMART, &state);
	if (ret)
		return ret;
	return !(state & 1) && !!(state & EVX_NAV) == enabled ? 0 : -EIO;
}

static void evx_apply(struct work_struct *work)
{
	int state, ret;

	mutex_lock(&evx_state_lock);
	ret = evx_read_int(EVX_SMART, &state);
	if (ret)
		goto done;
	/* Never take over a navigation limit already enabled by the ROM/user.
	 * Its threshold is not readable in the stock packed getter.
	 */
	if (evx_target && !evx_owned && (state & EVX_NAV)) {
		ret = -EBUSY;
		goto done;
	}
	if (!evx_target && !evx_owned)
		goto done;
	if (evx_target && evx_owned && (state & EVX_NAV))
		goto done;
	ret = evx_write_nav(evx_target);
	if (!ret)
		evx_owned = evx_target;
	else if (evx_target) {
		/* A failed readback must not leave an unowned charge pause behind. */
		int rollback = evx_write_nav(false);

		evx_owned = rollback != 0;
	}
done:
	evx_error = ret;
	evx_neutral_since = 0;
	evx_last_sample = 0;
	mutex_unlock(&evx_state_lock);
	if (evx_battery)
		power_supply_changed(evx_battery);
}
static DECLARE_WORK(evx_apply_work, evx_apply);

static ssize_t bypass_charging_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int state, ret;
	bool enabled;

	mutex_lock(&evx_state_lock);
	ret = evx_read_int(EVX_SMART, &state);
	enabled = !ret && evx_owned && (state & EVX_NAV);
	mutex_unlock(&evx_state_lock);
	return ret ? ret : sysfs_emit(buf, "%d\n", enabled);
}

static ssize_t bypass_charging_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	bool enabled;
	int ret = kstrtobool(buf, &enabled);

	if (ret)
		return ret;
	mutex_lock(&evx_command_lock);
	evx_target = enabled;
	schedule_work(&evx_apply_work);
	flush_work(&evx_apply_work);
	ret = evx_error;
	mutex_unlock(&evx_command_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(bypass_charging);
static struct device_attribute dev_attr_bypass_charge =
	__ATTR(bypass_charge, 0644, bypass_charging_show, bypass_charging_store);

static ssize_t bypass_charging_supported_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int state, cp;
	int ret = evx_read_int(EVX_SMART, &state);

	if (!ret)
		ret = evx_read_int(EVX_USB "cp_sm_run_state", &cp);
	return sysfs_emit(buf, "%d\n", !ret);
}
static DEVICE_ATTR_RO(bypass_charging_supported);

static ssize_t bypass_charging_active_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct power_supply *bms;
	union power_supply_propval battery_current;
	int state, cp, online, suspended, vbus, ret;
	bool neutral = false;

	mutex_lock(&evx_state_lock);
	ret = evx_read_int(EVX_SMART, &state);
	/* Inactive bypass needs no charge-pump, USB or battery-current I/O.
	 * Preserve the same inactive result and reset the evidence window;
	 * do not cache measurements once the owned navigation pause is on.
	 */
	if (ret || !evx_owned || !(state & EVX_NAV))
		goto inactive;
	if (!ret)
		ret = evx_read_int(EVX_USB "cp_sm_run_state", &cp);
	if (!ret)
		ret = evx_read_int(EVX_USB "online", &online);
	if (!ret)
		ret = evx_read_int(EVX_USB "input_suspend", &suspended);
	if (!ret)
		ret = evx_read_int(EVX_USB "pmic_vbus", &vbus);
	if (ret || cp || !online || suspended || vbus < 4400 || vbus > 6000)
		goto inactive;
	bms = power_supply_get_by_name("bms");
	if (!ret)
		ret = bms ? power_supply_get_property(bms,
			POWER_SUPPLY_PROP_CURRENT_NOW, &battery_current) : -ENODEV;
	if (bms)
		power_supply_put(bms);
	if (evx_last_sample && time_after(jiffies, evx_last_sample + 2 * HZ))
		evx_neutral_since = 0;
	evx_last_sample = jiffies;
	/* Measured neutral-current state, not a fabricated requested=active bit.
	 * When adapter power is inadequate, battery supplementation is reported
	 * honestly; the controller never forces battery isolation or powerpath ON.
	 */
	if (!ret && evx_owned && (state & EVX_NAV) && !cp && online &&
	    !suspended && vbus >= 4400 && vbus <= 6000 &&
	    battery_current.intval >= -100000 && battery_current.intval <= 100000) {
		if (!evx_neutral_since)
			evx_neutral_since = jiffies;
		neutral = time_after_eq(jiffies, evx_neutral_since + 3 * HZ);
	} else {
		evx_neutral_since = 0;
	}
	goto out;
inactive:
	evx_neutral_since = 0;
	evx_last_sample = jiffies;
out:
	mutex_unlock(&evx_state_lock);
	return sysfs_emit(buf, "%d\n", neutral);
}
static DEVICE_ATTR_RO(bypass_charging_active);

static ssize_t bypass_charging_diagnostics_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
		"api=3 backend=oem-navigation owned=%d last_error=%d evidence=battery-neutral-observation\n",
		READ_ONCE(evx_owned), READ_ONCE(evx_error));
}
static DEVICE_ATTR_RO(bypass_charging_diagnostics);

static struct attribute *evx_attrs[] = {
	&dev_attr_bypass_charging.attr, &dev_attr_bypass_charge.attr,
	&dev_attr_bypass_charging_supported.attr,
	&dev_attr_bypass_charging_active.attr,
	&dev_attr_bypass_charging_diagnostics.attr, NULL,
};
static const struct attribute_group evx_group = { .attrs = evx_attrs };
static void evx_attach(struct work_struct *work);
static DECLARE_DELAYED_WORK(evx_attach_work, evx_attach);

static void evx_attach(struct work_struct *work)
{
	int ret;

	evx_battery = power_supply_get_by_name("battery");
	if (!evx_battery) {
		if (evx_attach_attempts++ < 60)
			schedule_delayed_work(&evx_attach_work, 2 * HZ);
		return;
	}
	ret = sysfs_create_group(&evx_battery->dev.kobj, &evx_group);
	if (ret) {
		pr_err("evonix_bypass: OEM API attach failed: %d\n", ret);
		power_supply_put(evx_battery);
		evx_battery = NULL;
	}
}

static int __init evx_init(void)
{
	if (!of_machine_is_compatible("mediatek,MT6899"))
		return -ENODEV;
	schedule_delayed_work(&evx_attach_work, 0);
	return 0;
}
module_init(evx_init);
MODULE_AUTHOR("NEESCHAL");
MODULE_DESCRIPTION("EVONIX Rodin stock-policy charge pause (Image-only)");
MODULE_LICENSE("GPL");
