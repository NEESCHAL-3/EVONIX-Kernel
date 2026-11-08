// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2025 Oplus. All rights reserved.
 */

#include "internal.h"
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/of.h>
#include <drm/drm_panel.h>
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
#include <linux/msm_drm_notify.h>
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
#include <linux/mtk_disp_notify.h>
#endif

#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY) || IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static struct notifier_block fb_notif;
#elif IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static void *g_panel_cookie;
#endif

atomic_t display_off = ATOMIC_LONG_INIT(0);
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static struct drm_panel *get_active_panel(void)
{
	int i;
	int count;
	struct device_node *panel_node = NULL;
	struct drm_panel *panel = NULL;
	struct device_node *np = NULL;

	np = of_find_node_by_name(NULL, "oplus,dsi-display-dev");
	if (!np) {
		log_err("oplus,dsi-display-dev node missing\n");
		return NULL;
	}

	log_warn("oplus,dsi-display-dev node found\n");
	count = of_count_phandle_with_args(np, "oplus,dsi-panel-primary", NULL);
	if (count <= 0) {
		log_err("oplus,dsi-panel-primary missing\n");
		goto not_found;
	}

	for (i = 0; i < count; i++) {
		panel_node = of_parse_phandle(np, "oplus,dsi-panel-primary", i);
		panel = of_drm_find_panel(panel_node);
		of_node_put(panel_node);
		if (!IS_ERR(panel)) {
			log_warn("active panel found\n");
			goto found;
		}
	}
not_found:
	panel = NULL;
found:
	of_node_put(np);
	return panel;
}

static void bright_fb_notifier_callback(enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *client_data)
{
	if (!notification) {
		log_info("%s, invalid notify\n", __func__);
		return;
	}

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		atomic_set(&display_off, 1);
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		atomic_set(&display_off, 0);
		break;
	default:
		break;
	}
}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
static int bright_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;

	if (evdata && evdata->data) {
		blank = evdata->data;

		if (*blank ==  MSM_DRM_BLANK_POWERDOWN)
			atomic_set(&display_off, 1);
		else if (*blank == MSM_DRM_BLANK_UNBLANK)
			atomic_set(&display_off, 0);
	}

	return NOTIFY_OK;
}
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static int mtk_bright_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	int *blank = (int *)data;

	if (!blank) {
		log_err("get disp stat err, blank is NULL!\n");
		return 0;
	}

	if (*blank == MTK_DISP_BLANK_POWERDOWN)
		atomic_set(&display_off, 1);
	else if (*blank == MTK_DISP_BLANK_UNBLANK)
		atomic_set(&display_off, 0);
	return NOTIFY_OK;
}
#endif

void register_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	struct drm_panel *active_panel;
	void *cookie = NULL;

	active_panel = get_active_panel();
	if (active_panel)
		cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_MM, active_panel, bright_fb_notifier_callback, NULL);

	if (active_panel && !IS_ERR(cookie)) {
		log_warn("%s success\n", __func__);
		g_panel_cookie = cookie;
	} else {
		log_err("%s failed. need fix\n", __func__);
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	fb_notif.notifier_call = bright_fb_notifier_callback;
	if (msm_drm_register_client(&fb_notif))
		log_err("msm_drm_register_client failed\n");
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	fb_notif.notifier_call = mtk_bright_fb_notifier_callback;
	if (mtk_disp_notifier_register("Oplus_hybridswap", &fb_notif))
		log_err("mtk_disp_notifier_register failed\n");
#endif
}

void unregister_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	if (g_panel_cookie) {
		panel_event_notifier_unregister(g_panel_cookie);
		g_panel_cookie = NULL;
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	msm_drm_unregister_client(&fb_notif);
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	mtk_disp_notifier_unregister(&fb_notif);
#endif
}
