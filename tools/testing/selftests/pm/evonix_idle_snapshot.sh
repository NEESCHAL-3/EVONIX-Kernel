#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# One-shot observation only. Do not run this in a loop during an idle test.

set -eu

sample_root=
vendor_stat=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--root)
		[ "$#" -ge 2 ] || exit 2
		case "$2" in /*) sample_root=${2%/} ;; *) exit 2 ;; esac
		shift 2
		;;
	--vendor-stat) vendor_stat=1; shift ;;
	*) printf 'Usage: sh %s [--vendor-stat] [--root /fixture]\n' "$0" >&2; exit 2 ;;
	esac
done

# A stalled diagnostic node must not leave an observer running indefinitely.
for sample_command in timeout head; do
	command -v "$sample_command" >/dev/null 2>&1 || {
		printf 'Required command unavailable: %s\n' "$sample_command" >&2
		exit 1
	}
done

read_node()
{
	printf '\n@@ %s\n' "$1"
	if [ -r "$sample_root$1" ]; then
		if ! timeout -k 1 2 head -n 256 "$sample_root$1" 2>/dev/null; then
			printf '[read failed or timed out]\n'
		fi
	else
		printf '[unavailable]\n'
	fi
}

printf 'EVONIX_IDLE_SNAPSHOT_V1\n'
read_node /proc/sys/kernel/random/boot_id
read_node /proc/uptime
read_node /proc/version
read_node /sys/kernel/wakeup_reasons/last_resume_reason
read_node /sys/kernel/wakeup_reasons/last_suspend_time
read_node /sys/kernel/debug/wakeup_sources
# Do not read /sys/power/wakeup_count: its show handler may wait for active
# wake sources to relax. Observe the debug counters instead.
read_node /sys/power/state
read_node /sys/power/mem_sleep
read_node /sys/power/autosleep
read_node /sys/power/sync_on_suspend
for sample_node in "$sample_root"/sys/power/suspend_stats/*; do
	[ -f "$sample_node" ] || continue
	read_node "${sample_node#"$sample_root"}"
done
read_node /sys/module/workqueue/parameters/power_efficient
read_node /sys/module/rcutree/parameters/enable_rcu_lazy
read_node /sys/kernel/debug/sched/features
read_node /proc/sys/kernel/sched_pelt_multiplier
read_node /proc/interrupts
read_node /proc/softirqs

for sample_attr in online present possible; do
	read_node "/sys/devices/system/cpu/$sample_attr"
done
read_node /sys/devices/system/cpu/cpuidle/current_driver
read_node /sys/devices/system/cpu/cpuidle/current_governor_ro

# The target is Rodin's eight CPUs, not the 32-CPU GKI ABI ceiling.
for sample_cpu in 0 1 2 3 4 5 6 7; do
	for sample_state in "$sample_root"/sys/devices/system/cpu/cpu"$sample_cpu"/cpuidle/state*; do
		[ -d "$sample_state" ] || continue
		for sample_attr in name latency residency usage time rejected disable; do
			read_node "${sample_state#"$sample_root"}/$sample_attr"
		done
	done
done
for sample_policy in "$sample_root"/sys/devices/system/cpu/cpufreq/policy*; do
	[ -d "$sample_policy" ] || continue
	for sample_attr in related_cpus scaling_governor scaling_driver scaling_min_freq \
			scaling_max_freq cpuinfo_min_freq cpuinfo_max_freq stats/time_in_state \
			sugov_ext/up_rate_limit_us sugov_ext/down_rate_limit_us \
			schedutil/rate_limit_us; do
		read_node "${sample_policy#"$sample_root"}/$sample_attr"
	done
done

for sample_attr in capacity status current_now voltage_now charge_counter temp; do
	read_node "/sys/class/power_supply/battery/$sample_attr"
done
for sample_attr in online usb_type; do
	read_node "/sys/class/power_supply/usb/$sample_attr"
done

# No recursive device-tree walk and no forced runtime resumes. Bound output.
sample_devices=0
for sample_bus in platform i2c spi usb sdio scsi pci; do
	for sample_power in "$sample_root"/sys/bus/"$sample_bus"/devices/*/power; do
		[ -r "$sample_power/runtime_status" ] || continue
		sample_devices=$((sample_devices + 1))
		if [ "$sample_devices" -gt 128 ]; then
			printf '\n@@ runtime_pm_limit\n[truncated at 128 devices]\n'
			break
		fi
		for sample_attr in runtime_status control autosuspend_delay_ms \
				runtime_active_time runtime_suspended_time wakeup; do
			read_node "${sample_power#"$sample_root"}/$sample_attr"
		done
	done
	[ "$sample_devices" -le 128 ] || break
done

# Opt-in: this known MediaTek read handler queries firmware statistics and
# updates its diagnostic snapshot. Never read arbitrary vendor debug nodes.
if [ "$vendor_stat" -eq 1 ]; then
	read_node /sys/kernel/mtk_lpm/spm/system_stat
	read_node /proc/mtk_lpm/spm/system_stat
fi
printf '\n@@ capture_end_uptime\n'
if [ -r "$sample_root/proc/uptime" ]; then
	timeout -k 1 2 head -n 1 "$sample_root/proc/uptime" || true
fi
