// SPDX-License-Identifier: GPL-3.0
/*
 * Copyright (C) 2021-2024 Cedric Bellegarde <cedric.bellegarde@adishatz.org>.
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com> (cpu_input_boost/devfreq_boost)
 */

#define pr_fmt(fmt) "power_saver: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/msm_drm_notify.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/swap.h>
#include <linux/writeback.h>

#include <power/power_saver.h>

/* The sched_param struct is located elsewhere in newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
#include <uapi/linux/sched/types.h>
#endif

static unsigned int power_saver_cpu_min_little __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_CPU_MIN_FREQ_LITTLE;
static unsigned int power_saver_cpu_min_big __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_CPU_MIN_FREQ_BIG;
#ifdef CONFIG_PRIME_CPU_MASK
static unsigned int power_saver_cpu_min_prime __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_CPU_MIN_FREQ_PRIME;
#endif

static unsigned int power_saver_cpu_max_little __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_CPU_MAX_FREQ_LITTLE;
static unsigned int power_saver_cpu_max_big __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_CPU_MAX_FREQ_BIG;
#ifdef CONFIG_PRIME_CPU_MASK
static unsigned int power_saver_cpu_max_prime __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_CPU_MAX_FREQ_PRIME;
#endif

static unsigned int power_saver_cpu_max_snd_little __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_SND_CPU_MAX_FREQ_LITTLE;
static unsigned int power_saver_cpu_max_snd_big __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_SND_CPU_MAX_FREQ_BIG;
#ifdef CONFIG_PRIME_CPU_MASK
static unsigned int power_saver_cpu_max_snd_prime __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_SND_CPU_MAX_FREQ_PRIME;
#endif

static unsigned int power_saver_screen_on_dirty_expire_centisecs __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_DIRTY_EXPIRE_CENTISECS;
static unsigned int power_saver_screen_on_swappiness __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_SWAPPINESS;
static unsigned int power_saver_screen_on_dirty_writeback_centisecs __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_DIRTY_WRITEBACK_CENTISECS;
static unsigned int power_saver_screen_on_dirty_background_ratio __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_DIRTY_BACKGROUND_RATIO;
static unsigned int power_saver_screen_on_dirty_ratio __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_ON_DIRTY_RATIO;

static unsigned int power_saver_screen_off_dirty_expire_centisecs __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_DIRTY_EXPIRE_CENTISECS;
static unsigned int power_saver_screen_off_swappiness __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_SWAPPINESS;
static unsigned int power_saver_screen_off_dirty_writeback_centisecs __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_DIRTY_WRITEBACK_CENTISECS;
static unsigned int power_saver_screen_off_dirty_background_ratio __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_DIRTY_BACKGROUND_RATIO;
static unsigned int power_saver_screen_off_dirty_ratio __read_mostly =
    CONFIG_POWER_SAVER_SCREEN_OFF_DIRTY_RATIO;

static unsigned int devfreq_frequencies[DEVFREQ_MAX] __read_mostly = {
	CONFIG_POWER_SAVER_SCREEN_OFF_LLCC_DDR_BW,
	CONFIG_POWER_SAVER_SCREEN_OFF_DDR_LATFLOOR,
	CONFIG_POWER_SAVER_SCREEN_OFF_LLCC_DDR_LAT,
	CONFIG_POWER_SAVER_SCREEN_OFF_CPU_LLCC_BW,
	CONFIG_POWER_SAVER_SCREEN_OFF_CPU_LLCC_LAT
};

static unsigned int devfreq_frequencies_snd[DEVFREQ_MAX] __read_mostly = {
	CONFIG_POWER_SAVER_SCREEN_OFF_SND_LLCC_DDR_BW,
	CONFIG_POWER_SAVER_SCREEN_OFF_SND_DDR_LATFLOOR,
	CONFIG_POWER_SAVER_SCREEN_OFF_SND_LLCC_DDR_LAT,
	CONFIG_POWER_SAVER_SCREEN_OFF_SND_CPU_LLCC_BW,
	CONFIG_POWER_SAVER_SCREEN_OFF_SND_CPU_LLCC_LAT
};

struct devfreq_device {
	struct devfreq **devfreq;
	unsigned int count;
	enum devfreq_device_type type;
};

struct power_saver_drv {
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	wait_queue_head_t update_waitq;
	struct devfreq_device *devfreq_devices[DEVFREQ_MAX];
	bool screen_on;
	atomic_t streams;
};


struct power_saver_drv power_saver_drv __read_mostly = {
	.screen_on = false,
	.streams = ATOMIC_INIT(0),
	.update_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(power_saver_drv.update_waitq),
};

void power_saver_register_devfreq(struct devfreq *devfreq, const char *devname)
{
	struct devfreq_device *device;
	enum devfreq_device_type device_type;

	if (!strcmp(devname, "soc:qcom,cpu-llcc-ddr-bw")) {
		device_type = DEVFREQ_CPU_LLCC_DDR_BW;
	} else if (strstr(devname, "cpu-ddr-latfloor")) {
		device_type = DEVFREQ_CPU_DDR_LATFLOOR;
	} else if (strstr(devname, "llcc-ddr-lat")) {
		device_type = DEVFREQ_CPU_LLCC_DDR_LAT;
	} else if (strstr(devname, "cpu-llcc-bw")) {
		device_type = DEVFREQ_CPU_LLCC_BW;
	} else if (strstr(devname, "cpu-llcc-lat")) {
		device_type = DEVFREQ_CPU_LLCC_LAT;
	} else {
		pr_info("Ignoring %s", devname);
		return;
	}

        pr_info("Registering %s", devname);

	device = power_saver_drv.devfreq_devices[device_type];
	if (!device) {
		device = kmalloc(sizeof(struct devfreq_device), GFP_KERNEL);
		device->devfreq = NULL;
		device->count = 0;
		device->type = device_type;
		WRITE_ONCE(power_saver_drv.devfreq_devices[device_type], device);
	}

	if (!device) {
		pr_err("Can't allocate memory!");
		return;
	}

	device->devfreq = krealloc(device->devfreq, (device->count + 1) * sizeof(devfreq), GFP_KERNEL);
	device->devfreq[device->count++] = devfreq;
}
EXPORT_SYMBOL(power_saver_register_devfreq);

static unsigned int get_max_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;
	unsigned int streams = atomic_read(&power_saver_drv.streams);

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask)) {
		if (streams > 0) {
			freq = power_saver_cpu_max_snd_little;
		} else {
			freq = power_saver_cpu_max_little;
		}
#ifdef CONFIG_PRIME_CPU_MASK
	} else if (cpumask_test_cpu(policy->cpu, cpu_prime_mask)){
		if (streams > 0) {
			freq = power_saver_cpu_max_snd_prime;
		} else {
			freq = power_saver_cpu_max_prime;
		}
#endif
	} else {
		if (streams > 0) {
			freq = power_saver_cpu_max_snd_big;
		} else {
			freq = power_saver_cpu_max_big;
		}
	}
	return max(policy->cpuinfo.min_freq,
		   min(freq, policy->cpuinfo.max_freq));
}

static unsigned int get_min_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = power_saver_cpu_min_little;
#ifdef CONFIG_PRIME_CPU_MASK
	else if (cpumask_test_cpu(policy->cpu, cpu_prime_mask))
		freq = power_saver_cpu_min_prime;
#endif
	else
		freq = power_saver_cpu_min_big;

	return max(freq, policy->cpuinfo.min_freq);
}

static void update_vm_policy(void)
{
	if (power_saver_drv.screen_on) {
		vm_swappiness = power_saver_screen_on_swappiness;
		dirty_expire_interval = power_saver_screen_on_dirty_expire_centisecs;
		dirty_writeback_interval = power_saver_screen_on_dirty_writeback_centisecs;
		dirty_background_ratio = power_saver_screen_on_dirty_background_ratio;
		vm_dirty_ratio = power_saver_screen_on_dirty_ratio;
	} else {
		vm_swappiness = power_saver_screen_off_swappiness;
		dirty_expire_interval = power_saver_screen_off_dirty_expire_centisecs;
		dirty_writeback_interval = power_saver_screen_off_dirty_writeback_centisecs;
		dirty_background_ratio = power_saver_screen_off_dirty_background_ratio;
		vm_dirty_ratio = power_saver_screen_off_dirty_ratio;
	}
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	if (power_saver_cpu_min_little != 0 || power_saver_cpu_max_little != 0) {
		cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
		cpufreq_update_policy(cpu);
	}
	if (power_saver_cpu_min_big != 0 || power_saver_cpu_max_big != 0) {
		cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
		cpufreq_update_policy(cpu);
	}
#ifdef CONFIG_PRIME_CPU_MASK
	if (power_saver_cpu_min_prime != 0 || power_saver_cpu_max_prime != 0) {
		cpu = cpumask_first_and(cpu_prime_mask, cpu_online_mask);
		cpufreq_update_policy(cpu);
	}
#endif
	put_online_cpus();
}

static void update_devfreq_policy(void)
{
	struct devfreq_device *device;
	unsigned int (*frequencies)[DEVFREQ_MAX];
	int i, j;

	if (atomic_read(&power_saver_drv.streams) > 0)
		frequencies = &devfreq_frequencies_snd;
	else
		frequencies = &devfreq_frequencies;

	for (i = 0; i < DEVFREQ_MAX; i++) {
		if (power_saver_drv.devfreq_devices[i] == NULL)
			continue;

		device = power_saver_drv.devfreq_devices[i];
		for (j = 0; j < device->count ; j++) {
			mutex_lock(&device->devfreq[j]->lock);

			if (power_saver_drv.screen_on) {
				device->devfreq[j]->max_freq =
					device->devfreq[j]->profile->freq_table[
						device->devfreq[j]->profile->max_state - 1
					];
			} else {
				device->devfreq[j]->max_freq = (*frequencies)[device->type];
			}

			update_devfreq(device->devfreq[j]);
			mutex_unlock(&device->devfreq[j]->lock);
		}
	}
}

static int update_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct power_saver_drv *drv = data;
	bool screen_on = false;
	unsigned int streams = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool _screen_on;
		unsigned int _streams = streams;
		bool should_stop = false;

		wait_event(drv->update_waitq,
			   (_screen_on = READ_ONCE(drv->screen_on)) != screen_on ||
			   (_streams = atomic_read(&drv->streams)) != streams ||
			   (should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		screen_on = _screen_on;
		streams = _streams;
		update_online_cpu_policy();
		update_devfreq_policy();
		update_vm_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct power_saver_drv *drv = container_of(nb, typeof(*drv), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* When screen is on, use upper frequency */
	if (drv->screen_on == true) {
		policy->min = get_min_freq(policy);
		policy->max = policy->cpuinfo.max_freq;
		return NOTIFY_OK;
	}
	policy->max = get_max_freq(policy);
	policy->min = policy->cpuinfo.min_freq;
	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct power_saver_drv *drv =
	    container_of(nb, typeof(*drv), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	if (*blank == MSM_DRM_BLANK_UNBLANK) {
		drv->screen_on = true;
	} else if (*blank == MSM_DRM_BLANK_POWERDOWN) {
		drv->screen_on = false;
	}

	wake_up(&drv->update_waitq);
	return NOTIFY_OK;
}

static void sound_enabled(void)
{
	atomic_inc(&power_saver_drv.streams);

	wake_up(&power_saver_drv.update_waitq);
}

static void sound_disabled(void)
{
	if (atomic_read(&power_saver_drv.streams) == 0)
		return;

	atomic_dec(&power_saver_drv.streams);

	wake_up(&power_saver_drv.update_waitq);
}

struct power_saver power_saver __read_mostly = {
	.sound_enabled = sound_enabled,
	.sound_disabled = sound_disabled
};
EXPORT_SYMBOL(power_saver);

static int __init power_saver_init(void)
{
	struct power_saver_drv *drv = &power_saver_drv;
	struct task_struct *thread;
	int ret;
	int i;

	for (i = 0; i < DEVFREQ_MAX; i++) {
		drv->devfreq_devices[i] = NULL;
	}

	drv->cpu_notif.notifier_call = cpu_notifier_cb;
	ret =
	    cpufreq_register_notifier(&drv->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	drv->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	drv->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&drv->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm notifier, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	thread =
	    kthread_run_perf_critical(cpu_perf_mask, update_thread, drv,
				      "power_saver");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start Power Saver update thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

 unregister_fb_notif:
	msm_drm_unregister_client(&drv->msm_drm_notif);
 unregister_cpu_notif:
	cpufreq_unregister_notifier(&drv->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}

subsys_initcall(power_saver_init);
