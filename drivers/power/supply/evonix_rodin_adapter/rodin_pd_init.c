// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kallsyms.h>
#include "mtk_charger.h"
#include "rodin_pd_init.h"

void prepare_rodin_pd_notifier(struct adapter_device *adapter,
				    struct notifier_block *nb)
{
	unsigned long callback;
	struct info_notifier_block *ta_nb;
	struct mtk_charger *charger;

	/* These are the layouts used by Rodin's supplied charger modules. */
	static_assert(offsetof(struct mtk_charger, pd_adapter) == 1856);
	static_assert(offsetof(struct adapter_device, vdm_data.ta_version) == 0x628);
	callback = kallsyms_lookup_name("mtk_charger_framework:notify_adapter_event");
	if (!callback || (unsigned long)nb->notifier_call != callback)
		return;
	ta_nb = container_of(nb, struct info_notifier_block, nb);
	charger = ta_nb->info;
	if (!charger || adapter != READ_ONCE(charger->adapter_dev[0]))
		return;

	/* Match the reference owned by the charger's normal adapter lookup. */
	if (READ_ONCE(charger->pd_adapter))
		return;
	get_device(&adapter->dev);
	if (cmpxchg_release(&charger->pd_adapter, NULL, adapter))
		put_device(&adapter->dev);
}

