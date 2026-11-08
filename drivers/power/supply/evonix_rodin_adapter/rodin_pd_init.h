/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RODIN_PD_INIT_H
#define _RODIN_PD_INIT_H
struct adapter_device;
struct notifier_block;
void prepare_rodin_pd_notifier(struct adapter_device *adapter, struct notifier_block *nb);
#endif
