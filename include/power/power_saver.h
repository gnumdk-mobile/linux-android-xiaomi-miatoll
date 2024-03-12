// SPDX-License-Identifier: GPL-3.0
/*
 * Copyright (c) 2024 Cédric Bellegarde <cedric.bellegarde@adishatz.org>
 */

#ifndef _POWER_SAVER_H_
#define _POWER_SAVER_H_

#include <linux/devfreq.h>

enum devfreq_device_type {
	DEVFREQ_CPU_LLCC_DDR_BW,
	DEVFREQ_CPU_DDR_LATFLOOR,
	DEVFREQ_CPU_LLCC_DDR_LAT,
	DEVFREQ_CPU_LLCC_BW,
	DEVFREQ_CPU_LLCC_LAT,
	DEVFREQ_MAX
};


struct power_saver {
	void (*sound_enabled)(void);
	void (*sound_disabled)(void);
};

#ifdef CONFIG_POWER_SAVER
void power_saver_register_devfreq(struct devfreq *devfreq, const char *devname);
#else
static inline void power_saver_register_devfreq(struct devfreq *devfreq, const char *devname) {}
#endif

extern struct power_saver power_saver;

#endif /* _POWER_SAVER_H_ */
