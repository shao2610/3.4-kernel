/*
 * include/linux/melfas_ts.h - platform data structure for MCS Series sensor
 *
 * Copyright (C) 2010 Melfas, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _LINUX_MELFAS_TS_H
#define _LINUX_MELFAS_TS_H

#define MELFAS_TS_NAME "melfas-ts"


struct melfas_tsi_platform_data {
	uint32_t version;
	int max_x;
	int max_y;
	int max_pressure;
	int max_width;
	int gpio_scl;
	int gpio_sda;
	int i2c_int_gpio;
	unsigned short ic_booting_delay;		/* ms */
	int (*power)(int on);	/* Only valid in first array entry */
	int (*power_enable)(int en, bool log_en);
	unsigned char fw_ver;
	unsigned char manufcturer_id;
};

#endif /* _LINUX_MELFAS_TS_H */
