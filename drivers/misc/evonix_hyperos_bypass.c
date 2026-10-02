// SPDX-License-Identifier: GPL-2.0
/*
 * EVONIX HyperOS charger power-path bypass for Xiaomi rodin / MT6899.
 *
 * The charger backend, charge-pump stop sequence and guards are retained
 * from evonix_cos_power_compat.c. This independent initializer exposes the
 * same battery aliases without depending on ColorOS compatibility nodes.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define EVX_NAME "evonix_hyperos_bypass"

static struct power_supply *evx_bypass_battery_psy;
static bool evx_bypass_attrs_created;
static int evx_bypass_retry_count;

static void evx_bypass_retry_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(evx_bypass_retry_work, evx_bypass_retry_workfn);

/*
 * v31R5: real MTK charger-class backend.
 * Do NOT use battery/input_suspend; that cuts USB input and is fake bypass.
 * This only disables/enables charger IC charging while keeping power path alive.
 */
struct charger_device;

typedef struct charger_device *(*evx_get_charger_by_name_t)(const char *name);
typedef int (*evx_charger_dev_enable_t)(struct charger_device *chg_dev, bool en);
typedef int (*evx_charger_dev_enable_powerpath_t)(struct charger_device *chg_dev, bool en);


typedef int (*evx_charger_dev_cp_set_mode_t)(struct charger_device *chg_dev, int mode);
typedef int (*evx_charger_dev_cp_device_init_t)(struct charger_device *chg_dev, int mode);
typedef int (*evx_charger_dev_cp_enable_adc_t)(struct charger_device *chg_dev, bool en);
static evx_get_charger_by_name_t evx_get_charger_by_name_fn;
static evx_charger_dev_enable_t evx_charger_dev_enable_fn;
static evx_charger_dev_enable_powerpath_t evx_charger_dev_enable_powerpath_fn;


static evx_charger_dev_cp_set_mode_t evx_charger_dev_cp_set_mode_fn;
static evx_charger_dev_cp_device_init_t evx_charger_dev_cp_device_init_fn;
static evx_charger_dev_cp_enable_adc_t evx_charger_dev_cp_enable_adc_fn;
static struct charger_device *evx_cp_master_chgdev;
static bool evx_bypass_guard_active;
static bool evx_cp_guard_registered;

static struct kprobe evx_kp_cp_set_mode = {
	.symbol_name = "charger_dev_cp_set_mode",
};
static struct kprobe evx_kp_cp_device_init = {
	.symbol_name = "charger_dev_cp_device_init",
};
static struct kprobe evx_kp_cp_enable_adc = {
	.symbol_name = "charger_dev_cp_enable_adc",
};
static struct kprobe evx_kp_charger_enable = {
	.symbol_name = "charger_dev_enable",
};
static struct kprobe evx_kp_charger_powerpath = {
	.symbol_name = "charger_dev_enable_powerpath",
};
#ifdef CONFIG_KPROBES
typedef unsigned long (*evx_kallsyms_lookup_name_t)(const char *name);

static unsigned long evx_lookup_symbol_addr(const char *name)
{
        static evx_kallsyms_lookup_name_t lookup_fn;
        struct kprobe kp = {
                .symbol_name = "kallsyms_lookup_name",
        };
        int ret;

        if (!lookup_fn) {
                ret = register_kprobe(&kp);
                if (ret < 0 || !kp.addr) {
                        pr_warn(EVX_NAME ": kallsyms kprobe failed: %d\n", ret);
                        return 0;
                }

                lookup_fn = (evx_kallsyms_lookup_name_t)kp.addr;
                unregister_kprobe(&kp);
        }

        return lookup_fn ? lookup_fn(name) : 0;
}
#else
static unsigned long evx_lookup_symbol_addr(const char *name)
{
        return 0;
}
#endif

static struct charger_device *evx_primary_chgdev;
static bool evx_real_bypass_cached;



static int evx_resolve_charger_backend(void)
{
        const char * const names[] = {
                "primary_chg",
                "primary_charger",
                "mtk-master-charger",
                "mt6375-chg",
                "mt6375_chg",
        };
        int i;

        if (!evx_get_charger_by_name_fn) {
                evx_get_charger_by_name_fn =
                        (evx_get_charger_by_name_t)evx_lookup_symbol_addr("get_charger_by_name");
                if (!evx_get_charger_by_name_fn) {
                        pr_warn(EVX_NAME ": get_charger_by_name lookup failed\n");
                        return -EPROBE_DEFER;
                }
        }

        if (!evx_charger_dev_enable_fn) {
                evx_charger_dev_enable_fn =
                        (evx_charger_dev_enable_t)evx_lookup_symbol_addr("charger_dev_enable");
                if (!evx_charger_dev_enable_fn) {
                        pr_warn(EVX_NAME ": charger_dev_enable lookup failed\n");
                        return -EPROBE_DEFER;
                }
        }

        if (!evx_charger_dev_enable_powerpath_fn) {
                evx_charger_dev_enable_powerpath_fn =
                        (evx_charger_dev_enable_powerpath_t)evx_lookup_symbol_addr("charger_dev_enable_powerpath");
                if (!evx_charger_dev_enable_powerpath_fn)
                        pr_warn(EVX_NAME ": charger_dev_enable_powerpath lookup failed, continuing\n");
        }

        if (IS_ERR_OR_NULL(evx_primary_chgdev))
                evx_primary_chgdev = NULL;

        if (!evx_primary_chgdev) {
                for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                        evx_primary_chgdev = evx_get_charger_by_name_fn(names[i]);
                        if (!IS_ERR_OR_NULL(evx_primary_chgdev)) {
                                pr_info(EVX_NAME ": charger backend resolved: %s\n", names[i]);
                                break;
                        }
                        evx_primary_chgdev = NULL;
                }
        }

        if (!evx_primary_chgdev) {
                pr_warn(EVX_NAME ": no primary charger device found\n");
                return -ENODEV;
        }

        return 0;
}


static int evx_resolve_cp_master_backend(void)
{
	static const char * const names[] = {
		"cp_master",
		"sc858x-master",
		"sc858x_master",
		"bq25985-master",
		"bq25985_master",
	};
	int i;

	if (!evx_get_charger_by_name_fn) {
		evx_get_charger_by_name_fn =
			(evx_get_charger_by_name_t)evx_lookup_symbol_addr("get_charger_by_name");
		if (!evx_get_charger_by_name_fn) {
			pr_warn(EVX_NAME ": cp backend get_charger_by_name lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_charger_dev_cp_set_mode_fn) {
		evx_charger_dev_cp_set_mode_fn =
			(evx_charger_dev_cp_set_mode_t)evx_lookup_symbol_addr("charger_dev_cp_set_mode");
		if (!evx_charger_dev_cp_set_mode_fn) {
			pr_warn(EVX_NAME ": charger_dev_cp_set_mode lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_charger_dev_cp_device_init_fn) {
		evx_charger_dev_cp_device_init_fn =
			(evx_charger_dev_cp_device_init_t)evx_lookup_symbol_addr("charger_dev_cp_device_init");
		if (!evx_charger_dev_cp_device_init_fn) {
			pr_warn(EVX_NAME ": charger_dev_cp_device_init lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_charger_dev_cp_enable_adc_fn) {
		evx_charger_dev_cp_enable_adc_fn =
			(evx_charger_dev_cp_enable_adc_t)evx_lookup_symbol_addr("charger_dev_cp_enable_adc");
		if (!evx_charger_dev_cp_enable_adc_fn) {
			pr_warn(EVX_NAME ": charger_dev_cp_enable_adc lookup failed\n");
			return -ENOENT;
		}
	}

	if (!evx_cp_master_chgdev) {
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			evx_cp_master_chgdev = evx_get_charger_by_name_fn(names[i]);
			if (!IS_ERR_OR_NULL(evx_cp_master_chgdev)) {
				pr_info(EVX_NAME ": cp backend resolved: %s\n", names[i]);
				break;
			}
			evx_cp_master_chgdev = NULL;
		}
	}

	if (!evx_cp_master_chgdev) {
		pr_warn(EVX_NAME ": cp_master charger device not found\n");
		return -ENODEV;
	}

	return 0;
}

static inline bool evx_is_cp_master_arg(struct charger_device *chg)
{
	return evx_cp_master_chgdev && chg == evx_cp_master_chgdev;
}

static inline bool evx_is_primary_charger_arg(struct charger_device *chg)
{
	return evx_primary_chgdev && chg == evx_primary_chgdev;
}

static int evx_guard_cp_set_mode_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (evx_bypass_guard_active && evx_is_cp_master_arg(chg) && regs->regs[1] != 0) {
		regs->regs[1] = 0;
		pr_info(EVX_NAME ": guard forced cp_set_mode 0\n");
	}
	return 0;
}

static int evx_guard_cp_device_init_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (evx_bypass_guard_active && evx_is_cp_master_arg(chg) && regs->regs[1] != 0) {
		regs->regs[1] = 0;
		pr_info(EVX_NAME ": guard forced cp_device_init 0\n");
	}
	return 0;
}

static int evx_guard_cp_enable_adc_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (evx_bypass_guard_active && evx_is_cp_master_arg(chg) && regs->regs[1] != 0) {
		regs->regs[1] = 0;
		pr_info(EVX_NAME ": guard blocked cp_enable_adc true\n");
	}
	return 0;
}

static int evx_guard_charger_enable_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (!evx_bypass_guard_active)
		return 0;

	if (regs->regs[1] == 0)
		return 0;

	regs->regs[1] = 0;

	if (evx_is_cp_master_arg(chg))
		pr_info(EVX_NAME ": guard blocked cp_master enable true");
	else if (evx_is_primary_charger_arg(chg))
		pr_info(EVX_NAME ": guard blocked primary charger enable true");
	else
		pr_info(EVX_NAME ": guard blocked charger enable true chg=%px", chg);

	return 0;
}

static int evx_guard_powerpath_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct charger_device *chg = (struct charger_device *)regs->regs[0];

	if (!evx_bypass_guard_active)
		return 0;

	if (regs->regs[1] != 0)
		return 0;

	regs->regs[1] = 1;

	if (evx_is_primary_charger_arg(chg))
		pr_info(EVX_NAME ": guard forced primary powerpath true");
	else
		pr_info(EVX_NAME ": guard forced charger powerpath true chg=%px", chg);

	return 0;
}

static void evx_register_cp_master_guard(void)
{
	int ret;
	bool ok = false;

	if (evx_cp_guard_registered)
		return;

	evx_kp_cp_set_mode.pre_handler = evx_guard_cp_set_mode_pre;
	evx_kp_cp_device_init.pre_handler = evx_guard_cp_device_init_pre;
	evx_kp_cp_enable_adc.pre_handler = evx_guard_cp_enable_adc_pre;
	evx_kp_charger_enable.pre_handler = evx_guard_charger_enable_pre;
	evx_kp_charger_powerpath.pre_handler = evx_guard_powerpath_pre;

	ret = register_kprobe(&evx_kp_cp_set_mode);
	pr_info(EVX_NAME ": guard register cp_set_mode ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_cp_device_init);
	pr_info(EVX_NAME ": guard register cp_device_init ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_cp_enable_adc);
	pr_info(EVX_NAME ": guard register cp_enable_adc ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_charger_enable);
	pr_info(EVX_NAME ": guard register charger_enable ret=%d\n", ret);
	if (!ret) ok = true;

	ret = register_kprobe(&evx_kp_charger_powerpath);
	pr_info(EVX_NAME ": guard register charger_powerpath ret=%d\n", ret);
	if (!ret) ok = true;

	evx_cp_guard_registered = ok;
}

static void evx_try_stop_cp_master_for_bypass(void)
{
	int ret;

	ret = evx_resolve_cp_master_backend();
	if (ret) {
		pr_warn(EVX_NAME ": cp_master stop skipped ret=%d\n", ret);
		return;
	}

	evx_register_cp_master_guard();

	ret = evx_charger_dev_cp_enable_adc_fn(evx_cp_master_chgdev, false);
	pr_info(EVX_NAME ": cp_master enable_adc false ret=%d\n", ret);

	ret = evx_charger_dev_cp_set_mode_fn(evx_cp_master_chgdev, 0);
	pr_info(EVX_NAME ": cp_master set_mode 0 ret=%d\n", ret);

	ret = evx_charger_dev_cp_device_init_fn(evx_cp_master_chgdev, 0);
	pr_info(EVX_NAME ": cp_master device_init 0 ret=%d\n", ret);

	ret = evx_charger_dev_enable_fn(evx_cp_master_chgdev, false);
	pr_info(EVX_NAME ": cp_master enable false ret=%d\n", ret);

	msleep(500);
}

static int evx_real_bypass_set(bool enable)
{
        int ret;
        int pp_ret = 0;

        ret = evx_resolve_charger_backend();
        if (ret)
                return ret;


	if (enable) {
		evx_bypass_guard_active = true;
		evx_try_stop_cp_master_for_bypass();
	} else {
		evx_bypass_guard_active = false;
	}
        if (evx_charger_dev_enable_powerpath_fn) {
                pp_ret = evx_charger_dev_enable_powerpath_fn(evx_primary_chgdev, true);
                if (pp_ret)
                        pr_warn(EVX_NAME ": powerpath keep-on returned %d\n", pp_ret);
        }

        /*
         * Real bypass style:
         * enable=true  => stop battery charging only
         * enable=false => allow battery charging again
         */
        ret = evx_charger_dev_enable_fn(evx_primary_chgdev, !enable);
        if (ret) {
                pr_warn(EVX_NAME ": charger_dev_enable(%d) failed: %d\n", !enable, ret);
                return ret;
        }

        evx_real_bypass_cached = enable;
        pr_info(EVX_NAME ": charger-class bypass %s\n", enable ? "enabled" : "disabled");
        return 0;
}

static ssize_t bypass_charging_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
        return sysfs_emit(buf, "%d\n", evx_real_bypass_cached ? 1 : 0);
}

static ssize_t bypass_charging_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
        bool enable;
        int ret;

        ret = kstrtobool(buf, &enable);
        if (ret)
                return ret;

        ret = evx_real_bypass_set(enable);
        if (ret)
                return ret;

        return count;
}

static DEVICE_ATTR(bypass_charging, 0664, bypass_charging_show, bypass_charging_store);

static ssize_t bypass_charge_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return bypass_charging_show(dev, attr, buf);
}

static ssize_t bypass_charge_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return bypass_charging_store(dev, attr, buf, count);
}

static DEVICE_ATTR(bypass_charge, 0664, bypass_charge_show, bypass_charge_store);

static int evx_create_real_bypass_attrs(void)
{
	int ret;

	evx_bypass_battery_psy = power_supply_get_by_name("battery");
	if (!evx_bypass_battery_psy) {
		pr_warn(EVX_NAME ": battery power_supply not ready for bypass aliases\n");
		return 0;
	}

	ret = device_create_file(&evx_bypass_battery_psy->dev,
				 &dev_attr_bypass_charging);
	if (ret && ret != -EEXIST) {
		pr_warn(EVX_NAME ": bypass_charging create failed: %d\n", ret);
		goto err_put_psy;
	}

	ret = device_create_file(&evx_bypass_battery_psy->dev,
				 &dev_attr_bypass_charge);
	if (ret && ret != -EEXIST) {
		pr_warn(EVX_NAME ": bypass_charge create failed: %d\n", ret);
		device_remove_file(&evx_bypass_battery_psy->dev,
				   &dev_attr_bypass_charging);
		goto err_put_psy;
	}

	evx_bypass_attrs_created = true;

	pr_info(EVX_NAME ": loaded real battery-manager bypass aliases\n");
	return 0;

err_put_psy:
	power_supply_put(evx_bypass_battery_psy);
	evx_bypass_battery_psy = NULL;
	return 0;
}


static void evx_bypass_retry_workfn(struct work_struct *work)
{
	if (evx_bypass_attrs_created)
		return;

	evx_create_real_bypass_attrs();

	if (!evx_bypass_attrs_created && evx_bypass_retry_count++ < 30) {
		pr_info(EVX_NAME ": bypass aliases not ready, retry=%d\n",
			evx_bypass_retry_count);
		schedule_delayed_work(&evx_bypass_retry_work,
				      msecs_to_jiffies(2000));
	}
}

static void evx_remove_real_bypass_attrs(void)
{
	if (evx_bypass_attrs_created && evx_bypass_battery_psy) {
		device_remove_file(&evx_bypass_battery_psy->dev,
				   &dev_attr_bypass_charge);
		device_remove_file(&evx_bypass_battery_psy->dev,
				   &dev_attr_bypass_charging);
		evx_bypass_attrs_created = false;
	}

	if (evx_bypass_battery_psy) {
		power_supply_put(evx_bypass_battery_psy);
		evx_bypass_battery_psy = NULL;
	}
}



static int __init evx_hyperos_bypass_init(void)
{
	evx_create_real_bypass_attrs();
	if (!evx_bypass_attrs_created)
		schedule_delayed_work(&evx_bypass_retry_work,
				      msecs_to_jiffies(2000));

	return 0;
}

static void __exit evx_hyperos_bypass_exit(void)
{
	cancel_delayed_work_sync(&evx_bypass_retry_work);
	evx_remove_real_bypass_attrs();
}

module_init(evx_hyperos_bypass_init);
module_exit(evx_hyperos_bypass_exit);

MODULE_DESCRIPTION("EVONIX HyperOS charger power-path bypass");
MODULE_LICENSE("GPL");
