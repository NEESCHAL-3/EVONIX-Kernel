/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OPLUS_OSI_FREQ_H
#define OPLUS_OSI_FREQ_H
#include <linux/proc_fs.h>
int osi_freq_init(struct proc_dir_entry *pde);
void osi_freq_exit(struct proc_dir_entry *pde);
void osi_cpufreq_transition_handler(unsigned int frequency, unsigned int cpu);
void get_cpufreq_info(bool *is_sample);
#endif
