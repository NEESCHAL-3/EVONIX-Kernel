# EVONIX ColorOS real-backed compatibility layer
obj-y += evonix_cos_power_compat.o
obj-y += evonix_cos_perf_compat.o
obj-y += evonix_cos_qos_compat.o
obj-y += evonix_cos_afs_compat.o
obj-y += evonix_cos_proc_compat.o

ccflags-y += -I$(srctree)/drivers/soc/oplus/frame_boost
obj-$(CONFIG_EVONIX_OPLUS_FRAME_RATE) += rodin_frame_rate.o
obj-y += evonix_cos_cpu_tables.o
obj-$(CONFIG_EVONIX_OPLUS_TASK_ENERGY) += evonix_cos_task_energy.o
