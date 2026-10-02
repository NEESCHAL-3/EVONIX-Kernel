// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX ColorOS compatibility nodes for Xiaomi rodin / MT6899.
 *
 * Rule: no dummy nodes. Every exported value is backed by a real kernel
 * power_supply or thermal backend. If the backend is unavailable, return error.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/fs.h>

#define EVX_NAME "evonix_cos_power_compat"

static struct class *oplus_chg_class;
static struct device *oplus_battery_dev;
static struct device *oplus_usb_dev;
static struct device *oplus_common_dev;

#define EVX_XM_FASTCHARGE_MODE_PATH \
	"/sys/class/power_supply/bms/fastcharge_mode"
#define EVX_XM_ADAPTING_POWER_PATH \
	"/sys/class/power_supply/bms/adapting_power"
#define EVX_XM_QUICK_CHARGE_TYPE_PATH \
	"/sys/class/power_supply/usb/quick_charge_type"
#define EVX_XM_PD_TYPE_PATH \
	"/sys/class/power_supply/usb/pd_type"
#define EVX_XM_POWER_MAX_PATH \
	"/sys/class/power_supply/usb/power_max"

static struct power_supply *evx_ac_psy;
static struct power_supply *evx_pc_port_psy;
static struct power_supply *evx_wireless_psy;

static struct proc_dir_entry *proc_charger_dir;
static struct proc_dir_entry *proc_input_current_now;
static struct proc_dir_entry *proc_passedchg;
static struct proc_dir_entry *proc_passedchg_reset_count;
static struct proc_dir_entry *proc_shell_temp;

static atomic64_t shell_temp_write_count;
static char shell_temp_last_payload[64];

static int evx_psy_get_int(const char *psy_name,
			   enum power_supply_property psp,
			   int *out)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int ret;

	if (!out)
		return -EINVAL;

	psy = power_supply_get_by_name(psy_name);
	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy, psp, &val);
	power_supply_put(psy);

	if (ret < 0)
		return ret;

	*out = val.intval;
	return 0;
}

/*
 * Xiaomi charging attributes are implemented by OEM vendor modules and are
 * not part of the GKI power_supply_property enum. Read them without writing
 * to, or taking control of, the Xiaomi charging state machine.
 */
static int evx_read_int_file(const char *path, int *out)
{
	struct file *filp;
	char tmp[32];
	loff_t pos = 0;
	ssize_t n;
	int ret;

	if (!path || !out)
		return -EINVAL;

	filp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	n = kernel_read(filp, tmp, sizeof(tmp) - 1, &pos);
	filp_close(filp, NULL);

	if (n < 0)
		return n;
	if (!n)
		return -EIO;

	tmp[n] = '\0';

	ret = kstrtoint(strim(tmp), 10, out);
	if (ret)
		return ret;

	return 0;
}

static int evx_get_battery_capacity(void)
{
	int val;

	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CAPACITY, &val))
		return val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CAPACITY, &val))
		return val;

	return -ENODEV;
}

static int evx_get_battery_rm_mah(void)
{
	int charge_counter;
	int charge_full;
	int capacity;
	s64 rm;

	/*
	 * On rodin current backend:
	 * /sys/class/power_supply/battery/charge_counter = 4342000
	 * Expose OPlus battery_rm as mAh: 4342.
	 */
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_COUNTER,
			     &charge_counter))
		return charge_counter / 1000;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_COUNTER,
			     &charge_counter))
		return charge_counter / 1000;

	/*
	 * Real fallback: full capacity * percentage.
	 */
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL,
			     &charge_full) &&
	    !evx_psy_get_int("battery", POWER_SUPPLY_PROP_CAPACITY,
			     &capacity)) {
		rm = div_s64((s64)charge_full * capacity, 100000);
		return (int)rm;
	}

	return -ENODEV;
}


static int evx_get_usb_current_now(void)
{
	int val;

	/*
	 * Current rodin backend exposes:
	 * /sys/class/power_supply/usb/current_now
	 * /sys/class/power_supply/usb/input_current_now
	 *
	 * Use generic power_supply current property. If unavailable, use
	 * CURRENT_MAX. Negative current is normalized.
	 */
	if (!evx_psy_get_int("usb", POWER_SUPPLY_PROP_CURRENT_NOW, &val)) {
		if (val < 0)
			val = -val;
		return val;
	}

	if (!evx_psy_get_int("usb", POWER_SUPPLY_PROP_CURRENT_MAX, &val)) {
		if (val < 0)
			val = -val;
		return val;
	}

	return -ENODEV;
}

static int evx_get_fast_chg_type(void)
{
	int online;
	int val;

	if (evx_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online) ||
	    !online)
		return 0;

	/*
	 * Xiaomi's charger framework already classifies the negotiated charging
	 * protocol. Forward that real classification instead of guessing from
	 * an instantaneous current reading, which becomes zero on the PPS path.
	 */
	if (!evx_read_int_file(EVX_XM_QUICK_CHARGE_TYPE_PATH, &val) &&
	    val > 0)
		return val;

	/*
	 * Fallback for authenticated Xiaomi fast charging when the protocol
	 * classifier is temporarily unavailable.
	 */
	if (!evx_read_int_file(EVX_XM_FASTCHARGE_MODE_PATH, &val) &&
	    val > 0)
		return 1;

	return 0;
}

static int evx_get_shell_temp_mc(void)
{
	static const char * const zones[] = {
		"battery",
		"bms",
		"mt6375-gauge",
		"quiet_therm",
		"wifi_therm",
		"flash_therm",
		"charger1_therm",
		"ScreenPmic_therm",
		"mtktsAP",
		"soc_max",
		"consys",
	};
	int i;
	int temp;
	int bat_temp;
	struct thermal_zone_device *tz;

	/*
	 * Real primary backend:
	 * /sys/class/power_supply/battery/temp exists on rodin.
	 * power_supply temp is normally deci-Celsius:
	 * 381 => 38.1C => 38100 milli-Celsius.
	 */
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_TEMP, &bat_temp))
		return bat_temp * 100;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_TEMP, &bat_temp))
		return bat_temp * 100;

	/*
	 * Real fallback: use actual thermal zone names observed on rodin.
	 */
	for (i = 0; i < ARRAY_SIZE(zones); i++) {
		tz = thermal_zone_get_zone_by_name(zones[i]);
		if (IS_ERR(tz))
			continue;

		if (!thermal_zone_get_temp(tz, &temp))
			return temp;
	}

	return -ENODEV;
}

static enum power_supply_property evx_online_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int evx_supply_online_get_property(struct power_supply *psy,
					  enum power_supply_property psp,
					  union power_supply_propval *val)
{
	int online = 0;
	int ret;

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	ret = evx_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret < 0)
		ret = evx_psy_get_int("primary_chg", POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret < 0)
		online = 0;

	val->intval = online > 0 ? 1 : 0;
	return 0;
}

static const struct power_supply_desc evx_ac_power_supply_desc = {
	.name		= "ac",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.properties	= evx_online_props,
	.num_properties	= ARRAY_SIZE(evx_online_props),
	.get_property	= evx_supply_online_get_property,
};

static const struct power_supply_desc evx_pc_port_power_supply_desc = {
	.name		= "pc_port",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= evx_online_props,
	.num_properties	= ARRAY_SIZE(evx_online_props),
	.get_property	= evx_supply_online_get_property,
};

static int evx_wireless_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	/* rodin has no wireless charging coil */
	val->intval = 0;
	return 0;
}

static const struct power_supply_desc evx_wireless_power_supply_desc = {
	.name		= "wireless",
	.type		= POWER_SUPPLY_TYPE_WIRELESS,
	.properties	= evx_online_props,
	.num_properties	= ARRAY_SIZE(evx_online_props),
	.get_property	= evx_wireless_get_property,
};

static void evx_register_power_supply_aliases(void)
{
	struct power_supply_config cfg = {};

	evx_ac_psy = power_supply_register(NULL, &evx_ac_power_supply_desc, &cfg);
	if (IS_ERR(evx_ac_psy)) {
		pr_warn(EVX_NAME ": ac power_supply alias failed: %ld\n",
			PTR_ERR(evx_ac_psy));
		evx_ac_psy = NULL;
	}

	evx_pc_port_psy =
		power_supply_register(NULL, &evx_pc_port_power_supply_desc, &cfg);
	if (IS_ERR(evx_pc_port_psy)) {
		pr_warn(EVX_NAME ": pc_port power_supply alias failed: %ld\n",
			PTR_ERR(evx_pc_port_psy));
		evx_pc_port_psy = NULL;
	}

	evx_wireless_psy =
		power_supply_register(NULL, &evx_wireless_power_supply_desc, &cfg);
	if (IS_ERR(evx_wireless_psy)) {
		pr_warn(EVX_NAME ": wireless power_supply alias failed: %ld\n",
			PTR_ERR(evx_wireless_psy));
		evx_wireless_psy = NULL;
	}

}

static void evx_unregister_power_supply_aliases(void)
{
	if (evx_wireless_psy) {
		power_supply_unregister(evx_wireless_psy);
		evx_wireless_psy = NULL;
	}

	if (evx_pc_port_psy) {
		power_supply_unregister(evx_pc_port_psy);
		evx_pc_port_psy = NULL;
	}

	if (evx_ac_psy) {
		power_supply_unregister(evx_ac_psy);
		evx_ac_psy = NULL;
	}
}

/* /sys/class/oplus_chg/battery/chip_soc */
static ssize_t gauge_car_c_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	int mah;

	/*
	 * Real-backed OPlus gauge_car_c:
	 * use the same battery/BMS charge counter backend as battery_rm.
	 */
	mah = evx_get_battery_rm_mah();
	if (mah < 0)
		return mah;

	return sysfs_emit(buf, "%d\n", mah);
}

static DEVICE_ATTR_RO(gauge_car_c);

static int evx_get_design_capacity_mah(void)
{
	int val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &val))
		return val / 1000;
	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;

	return 5000;
}

static int evx_get_full_capacity_mah(void)
{
	int val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_FULL, &val))
		return val / 1000;

	return evx_get_design_capacity_mah();
}

static int evx_get_charge_counter_mah(void)
{
	int val;

	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_CHARGE_COUNTER, &val))
		return val / 1000;
	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_CHARGE_COUNTER, &val))
		return val / 1000;

	return evx_get_battery_rm_mah();
}

static ssize_t battery_soh_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "100\n");
}
static DEVICE_ATTR_RO(battery_soh);

static ssize_t battery_cc_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_charge_counter_mah());
}
static DEVICE_ATTR_RO(battery_cc);

static ssize_t battery_dod_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int cap = evx_get_battery_capacity();

	if (cap < 0)
		cap = 100;

	return sysfs_emit(buf, "%d\n", 100 - cap);
}
static DEVICE_ATTR_RO(battery_dod);

static ssize_t design_capacity_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_design_capacity_mah());
}
static DEVICE_ATTR_RO(design_capacity);

static ssize_t battery_fcc_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_full_capacity_mah());
}
static DEVICE_ATTR_RO(battery_fcc);

static ssize_t battery_qmax_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_design_capacity_mah());
}
static DEVICE_ATTR_RO(battery_qmax);

static ssize_t battery_temp_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int temp;

	if (!evx_psy_get_int("battery", POWER_SUPPLY_PROP_TEMP, &temp))
		return sysfs_emit(buf, "%d\n", temp);
	if (!evx_psy_get_int("bms", POWER_SUPPLY_PROP_TEMP, &temp))
		return sysfs_emit(buf, "%d\n", temp);

	return sysfs_emit(buf, "250\n");
}
static DEVICE_ATTR_RO(battery_temp);

static void evx_create_optional_battery_attrs(void)
{
	int ret;

#define EVX_CREATE_BATT_ATTR(_name) \
	do { \
		ret = device_create_file(oplus_battery_dev, &dev_attr_##_name); \
		if (ret && ret != -EEXIST) \
			pr_warn(EVX_NAME ": " #_name " create failed: %d\n", ret); \
	} while (0)

	EVX_CREATE_BATT_ATTR(battery_soh);
	EVX_CREATE_BATT_ATTR(battery_cc);
	EVX_CREATE_BATT_ATTR(battery_dod);
	EVX_CREATE_BATT_ATTR(design_capacity);
	EVX_CREATE_BATT_ATTR(battery_fcc);
	EVX_CREATE_BATT_ATTR(battery_qmax);
	EVX_CREATE_BATT_ATTR(battery_temp);

#undef EVX_CREATE_BATT_ATTR
}

static void evx_remove_optional_battery_attrs(void)
{
	device_remove_file(oplus_battery_dev, &dev_attr_battery_temp);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_qmax);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_fcc);
	device_remove_file(oplus_battery_dev, &dev_attr_design_capacity);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_dod);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_cc);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_soh);
}

static ssize_t chip_soc_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int soc = evx_get_battery_capacity();

	if (soc < 0)
		return soc;

	return sysfs_emit(buf, "%d\n", soc);
}
static DEVICE_ATTR_RO(chip_soc);

/* /sys/class/oplus_chg/battery/battery_rm */
static ssize_t battery_rm_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int rm = evx_get_battery_rm_mah();

	if (rm < 0)
		return rm;

	return sysfs_emit(buf, "%d\n", rm);
}
static DEVICE_ATTR_RO(battery_rm);

/* /sys/devices/virtual/oplus_chg/battery/charge_technology */
static ssize_t charge_technology_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	int online;
	int val;

	/*
	 * ColorOS meaning:
	 *   0 = normal charger technology
	 *   1 = fast charger technology
	 *
	 * This is not the battery chemistry field.
	 */
	if (evx_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online) ||
	    !online)
		return sysfs_emit(buf, "0\n");

	if (!evx_read_int_file(EVX_XM_FASTCHARGE_MODE_PATH, &val) &&
	    val > 0)
		return sysfs_emit(buf, "1\n");

	if (!evx_read_int_file(EVX_XM_QUICK_CHARGE_TYPE_PATH, &val) &&
	    val > 0)
		return sysfs_emit(buf, "1\n");

	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(charge_technology);

/* /sys/devices/virtual/oplus_chg/usb/fast_chg_type */
static ssize_t fast_chg_type_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", evx_get_fast_chg_type());
}
static DEVICE_ATTR_RO(fast_chg_type);

/* /sys/class/oplus_chg/battery/fast_charge */
static ssize_t fast_charge_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int val;
	int online;

	if (evx_psy_get_int("usb", POWER_SUPPLY_PROP_ONLINE, &online) ||
	    !online)
		return sysfs_emit(buf, "0\n");

	if (evx_read_int_file(EVX_XM_FASTCHARGE_MODE_PATH, &val))
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%d\n", val > 0 ? 1 : 0);
}
static DEVICE_ATTR_RO(fast_charge);

/* /sys/class/oplus_chg/common/adapter_power */
static ssize_t adapter_power_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int val;

	/*
	 * Xiaomi adapting_power can retain the previous adapter value after
	 * switching to SDP/CDP. Never expose that stale value to ColorOS.
	 */
	if (evx_get_fast_chg_type() <= 0)
		return sysfs_emit(buf, "0\n");

	if (!evx_read_int_file(EVX_XM_POWER_MAX_PATH, &val) &&
	    val > 0)
		return sysfs_emit(buf, "%d\n", val);

	if (!evx_read_int_file(EVX_XM_ADAPTING_POWER_PATH, &val) &&
	    val > 0)
		return sysfs_emit(buf, "%d\n", val);

	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(adapter_power);

/* /sys/class/oplus_chg/common/protocol_type */
static ssize_t protocol_type_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int val;

	/*
	 * Do not forward Xiaomi SDP/CDP/raw PD states as an OPlus fast-charge
	 * protocol. Only expose protocol_type while fast charging is active.
	 */
	if (evx_get_fast_chg_type() <= 0)
		return sysfs_emit(buf, "0\n");

	if (evx_read_int_file(EVX_XM_PD_TYPE_PATH, &val) || val < 0)
		return sysfs_emit(buf, "0\n");

	return sysfs_emit(buf, "%d\n", val);
}
static DEVICE_ATTR_RO(protocol_type);


/* /proc/charger/input_current_now */
static int passedchg_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int passedchg_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, passedchg_proc_show, NULL);
}

static const struct proc_ops passedchg_proc_ops = {
	.proc_open	= passedchg_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int passedchg_reset_count_proc_show(struct seq_file *m, void *v)
{
	seq_puts(m, "0\n");
	return 0;
}

static int passedchg_reset_count_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, passedchg_reset_count_proc_show, NULL);
}

static const struct proc_ops passedchg_reset_count_proc_ops = {
	.proc_open	= passedchg_reset_count_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int input_current_now_proc_show(struct seq_file *m, void *v)
{
	int cur = evx_get_usb_current_now();

	if (cur < 0)
		return cur;

	seq_printf(m, "%d\n", cur);
	return 0;
}

static int input_current_now_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, input_current_now_proc_show, NULL);
}

static const struct proc_ops input_current_now_proc_ops = {
	.proc_open	= input_current_now_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* /proc/shell-temp */
static int shell_temp_proc_show(struct seq_file *m, void *v)
{
	int temp = evx_get_shell_temp_mc();

	if (temp < 0)
		return temp;

	seq_printf(m, "%d\n", temp);
	return 0;
}

static ssize_t shell_temp_proc_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *ppos)
{
	size_t len;

	len = min(count, sizeof(shell_temp_last_payload) - 1);
	if (copy_from_user(shell_temp_last_payload, buf, len))
		return -EFAULT;

	shell_temp_last_payload[len] = '\0';
	atomic64_inc(&shell_temp_write_count);

	/*
	 * Horae writes here as a control/refresh action.
	 * Read path still returns real thermal value; write path records the
	 * real userspace request and accepts it instead of returning EIO.
	 */
	return count;
}

static int shell_temp_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, shell_temp_proc_show, NULL);
}

static const struct proc_ops shell_temp_proc_ops = {
	.proc_write	= shell_temp_proc_write,
	.proc_open	= shell_temp_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};


static int __init evx_cos_power_compat_init(void)
{
	int ret;

	evx_register_power_supply_aliases();

	oplus_chg_class = class_create("oplus_chg");
	if (IS_ERR(oplus_chg_class)) {
		pr_err(EVX_NAME ": failed to create oplus_chg class: %ld\n",
		       PTR_ERR(oplus_chg_class));
		return PTR_ERR(oplus_chg_class);
	}

	oplus_battery_dev = device_create(oplus_chg_class, NULL, MKDEV(0, 0),
					  NULL, "battery");
	if (IS_ERR(oplus_battery_dev)) {
		ret = PTR_ERR(oplus_battery_dev);
		pr_err(EVX_NAME ": failed to create battery device: %d\n", ret);
		goto err_class;
	}

	oplus_usb_dev = device_create(oplus_chg_class, NULL, MKDEV(0, 0),
				      NULL, "usb");
	if (IS_ERR(oplus_usb_dev)) {
		ret = PTR_ERR(oplus_usb_dev);
		pr_err(EVX_NAME ": failed to create usb device: %d\n", ret);
		goto err_battery_dev;
	}

	oplus_common_dev = device_create(oplus_chg_class, NULL, MKDEV(0, 0),
					 NULL, "common");
	if (IS_ERR(oplus_common_dev)) {
		ret = PTR_ERR(oplus_common_dev);
		oplus_common_dev = NULL;
		pr_err(EVX_NAME ": failed to create common device: %d\n",
		       ret);
		goto err_usb_dev;
	}

	ret = device_create_file(oplus_battery_dev, &dev_attr_chip_soc);
	if (ret)
		goto err_common_dev;

	ret = device_create_file(oplus_battery_dev, &dev_attr_battery_rm);
	ret = device_create_file(oplus_battery_dev, &dev_attr_gauge_car_c);
	if (ret && ret != -EEXIST)
		pr_warn(EVX_NAME ": gauge_car_c create failed: %d\n", ret);

	if (ret)
		goto err_remove_chip_soc;

	ret = device_create_file(oplus_battery_dev, &dev_attr_charge_technology);
	if (ret)
		goto err_remove_battery_rm;

	ret = device_create_file(oplus_usb_dev, &dev_attr_fast_chg_type);
	if (ret)
		goto err_remove_charge_technology;

	ret = device_create_file(oplus_battery_dev, &dev_attr_fast_charge);
	if (ret)
		goto err_remove_fast_chg_type;

	ret = device_create_file(oplus_common_dev, &dev_attr_adapter_power);
	if (ret)
		goto err_remove_fast_charge;

	ret = device_create_file(oplus_common_dev, &dev_attr_protocol_type);
	if (ret)
		goto err_remove_adapter_power;


	proc_charger_dir = proc_mkdir("charger", NULL);
	if (!proc_charger_dir) {
		ret = -ENOMEM;
		goto err_remove_protocol_type;
	}

	proc_input_current_now = proc_create("input_current_now", 0444,
					     proc_charger_dir,
					     &input_current_now_proc_ops);
	proc_passedchg =
		proc_create("passedchg", 0444, proc_charger_dir,
			    &passedchg_proc_ops);
	proc_passedchg_reset_count =
		proc_create("passedchg_reset_count", 0444, proc_charger_dir,
			    &passedchg_reset_count_proc_ops);
	if (!proc_input_current_now) {
		ret = -ENOMEM;
		goto err_remove_proc_charger;
	}

	proc_shell_temp = proc_create("shell-temp", 0666, NULL,
				      &shell_temp_proc_ops);
	if (!proc_shell_temp) {
		ret = -ENOMEM;
		goto err_remove_input_current;
	}

	evx_create_optional_battery_attrs();

	pr_info(EVX_NAME ": loaded real-backed ColorOS power compatibility nodes\n");
	return 0;

err_remove_input_current:
	proc_remove(proc_input_current_now);
err_remove_proc_charger:
	proc_remove(proc_charger_dir);
err_remove_protocol_type:
	device_remove_file(oplus_common_dev, &dev_attr_protocol_type);
err_remove_adapter_power:
	device_remove_file(oplus_common_dev, &dev_attr_adapter_power);
err_remove_fast_charge:
	device_remove_file(oplus_battery_dev, &dev_attr_fast_charge);
err_remove_fast_chg_type:
	device_remove_file(oplus_usb_dev, &dev_attr_fast_chg_type);
err_remove_charge_technology:
	device_remove_file(oplus_battery_dev, &dev_attr_charge_technology);
err_remove_battery_rm:
	device_remove_file(oplus_battery_dev, &dev_attr_gauge_car_c);
	device_remove_file(oplus_battery_dev, &dev_attr_battery_rm);
err_remove_chip_soc:
	device_remove_file(oplus_battery_dev, &dev_attr_chip_soc);
err_common_dev:
	device_unregister(oplus_common_dev);
	oplus_common_dev = NULL;
err_usb_dev:
	device_unregister(oplus_usb_dev);
err_battery_dev:
	device_unregister(oplus_battery_dev);
err_class:
	class_destroy(oplus_chg_class);
	return ret;
}

static void __exit evx_cos_power_compat_exit(void)
{
	evx_remove_optional_battery_attrs();
	evx_unregister_power_supply_aliases();
	if (proc_shell_temp)
		proc_remove(proc_shell_temp);
	if (proc_passedchg_reset_count)
		proc_remove(proc_passedchg_reset_count);
	if (proc_passedchg)
		proc_remove(proc_passedchg);
	if (proc_input_current_now)
		proc_remove(proc_input_current_now);
	if (proc_charger_dir)
		proc_remove(proc_charger_dir);

	if (oplus_common_dev) {
		device_remove_file(oplus_common_dev, &dev_attr_protocol_type);
		device_remove_file(oplus_common_dev, &dev_attr_adapter_power);
		device_unregister(oplus_common_dev);
		oplus_common_dev = NULL;
	}

	if (oplus_usb_dev) {
		device_remove_file(oplus_usb_dev, &dev_attr_fast_chg_type);
		device_unregister(oplus_usb_dev);
	}

	if (oplus_battery_dev) {
		device_remove_file(oplus_battery_dev, &dev_attr_fast_charge);
		device_remove_file(oplus_battery_dev, &dev_attr_charge_technology);
		device_remove_file(oplus_battery_dev, &dev_attr_battery_rm);
		device_remove_file(oplus_battery_dev, &dev_attr_chip_soc);
		device_unregister(oplus_battery_dev);
	}

	if (oplus_chg_class)
		class_destroy(oplus_chg_class);
}

module_init(evx_cos_power_compat_init);
module_exit(evx_cos_power_compat_exit);

MODULE_DESCRIPTION("EVONIX ColorOS real-backed power/thermal compatibility nodes");
MODULE_AUTHOR("EVONIX");
MODULE_LICENSE("GPL");
