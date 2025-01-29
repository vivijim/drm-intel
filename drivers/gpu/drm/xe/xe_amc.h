/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_AMC_H_
#define _XE_AMC_H_

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/irqreturn.h>
#include <linux/platform_device.h>
#include <linux/property.h>

struct xe_tile;
struct xe_device;

#define AMC_I2C_COOKIE 0xde

/**
 * struct amc_i2c_endpoint - AMC I2C endpoint information
 * If the cookie exists, the data is valid.
 *
 * @AMC_I2C_IRQ: indicate that the HW supports inerrupts
 */
struct amc_i2c_endpoint {
	union {
		u32 raw;
		struct {
			u8 cookie;
			u8 capabilities;
#define AMC_I2C_IRQ BIT(0)
#define AMC_I2C_PARITY BIT(1)
			u8 Rsvd[2];
		};
	} discovery;
	u32 address;
};

struct amc_i2c_info {
	/** @endpoint: Discovered I2C endpoint information */
	struct amc_i2c_endpoint endpoint;
	/** sw_node: used to connect to the Designware platform i2c bus */
	struct software_node sw_node;
	/** i2c_dev: keep track of the device */
	struct platform_device *i2c_dev;
	/** clk: Reference to created clock */
	struct clk *clk;
	/** clk: Reference to created clock lookup */
	struct clk_lookup *clock;
	/** i2c_name: clearly identify the usage */
	char i2c_name[32];
};

void amc_i2c_probe(struct xe_tile *tile);
void amc_i2c_remove(struct xe_device *xe);

irqreturn_t amc_irq_handler(int irq, void *arg);

#endif
