# SPDX-License-Identifier: GPL-2.0-only

subdir-ccflags-y += -I$(srctree)

obj-$(CONFIG_OPLUS_FEATURE_GAME_OPT) += oplus_game_opt.o

oplus_game_opt-y := \
	game_ctrl.o \
	cpu_load.o \
	cpufreq_limits.o \
	debug.o \
	early_detect.o \
	fake_cpufreq.o \
	rt_info.o \
	task_util.o \
	yield_opt.o \
	frame_sync.o \
	task_boost/heavy_task_boost.o \
	task_boost/boost_proc.o \
	critical_task_boost.o
