// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Add in Management Card (AMC) I2C adapter driver
 *
 * Copyright (C) 2023 Intel Corporation.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "regs/xe_regs.h"
#include "regs/xe_gt_regs.h"

#include "xe_amc.h"
#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_mmio.h"

static int amc_parity_check(struct xe_tile *tile)
{
	/* HW info required */
	return 0;
}

/*
 * According to the HW spec for the AMC, every time this register is used,
 * the bridge needs to be check for a parity error.
 *
 * Mirror the definition from i2c-designware-core.h
 */
#define DW_I2C_DATA_CMD 0x10

static int amc_i2c_read(void *context, unsigned int reg, unsigned int *val)
{
	struct xe_tile *tile = context;
	struct xe_reg xe_reg = {};

	xe_reg.addr = reg + I2C_BASE_OFFSET;
	*val = xe_mmio_read32(tile->primary_gt, xe_reg);

	if ((tile->amc.endpoint.discovery.capabilities & AMC_I2C_PARITY) &&
	    reg == DW_I2C_DATA_CMD)
		return amc_parity_check(tile);

	return 0;
}

static int amc_i2c_write(void *context, unsigned int reg, unsigned int val)
{
	struct xe_tile *tile = context;
	struct xe_reg xe_reg = {};

	xe_reg.addr = reg + I2C_BASE_OFFSET;

	xe_mmio_write32(tile->primary_gt, xe_reg, val);

	if ((tile->amc.endpoint.discovery.capabilities & AMC_I2C_PARITY) &&
	    reg == DW_I2C_DATA_CMD)
		return amc_parity_check(tile);

	return 0;
}

static const struct regmap_config i2c_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_read = amc_i2c_read,
	.reg_write = amc_i2c_write,
	.fast_io = true,
};

static const struct property_entry amc_i2c_properties[] = {
	PROPERTY_ENTRY_BOOL("amc,i2c-snps-model"),
	PROPERTY_ENTRY_U32("clock-frequency", I2C_MAX_FAST_MODE_PLUS_FREQ),
	{}
};

static void amc_unregister_clock_tree(struct clk *clk)
{
	struct clk *parent;

	while (clk) {
		parent = clk_get_parent(clk);
		clk_unregister(clk);
		clk = parent;
	}
}

static void amc_unregister_clock(struct amc_i2c_info *amc)
{
	if (!amc->clk)
		return;

	clkdev_drop(amc->clock);
	amc_unregister_clock_tree(amc->clk);
}

static int amc_register_clock(struct device *dev, struct amc_i2c_info *amc, u32 id)
{
	char clk_name[32];
	struct clk *clk;

	sprintf(clk_name, "i2c_designware.%d", id);

	/*
	 * The requested values for fp_hcnt:fsp_lcnt are 72 and 160.  This
	 * clock value calculates 72:106.  To match the lcnt value, the clock
	 * should be 201250000.
	 */
	clk = clk_register_fixed_rate(NULL, clk_name, NULL, 0, 133928000);

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	amc->clock = clkdev_create(clk, NULL, clk_name);
	if (!amc->clock) {
		amc_unregister_clock_tree(clk);
		return -ENOMEM;
	}

	amc->clk = clk;

	return 0;
}

/**
 * amc_i2c_probe - check to see if the AMC is present on the tile and
 * add the master i2c if necessary.
 * @tile: valid Xe tile instance
 *
 * Read the relevant regs to check for AMC availability and initailize the
 * data structure for later use.
 */
void amc_i2c_probe(struct xe_tile *tile)
{
	struct software_node *sw_node = &tile->amc.sw_node;
	struct amc_i2c_endpoint *ep = &tile->amc.endpoint;
	struct platform_device_info info = {};
	struct platform_device *i2c_dev;
	struct regmap *i2c_regmap;
	int ret;
	u32 id;

	ep->discovery.raw = xe_mmio_read32(tile->primary_gt, CLIENT_DISC_COOKIE);
	if (ep->discovery.cookie != AMC_I2C_COOKIE)
		return;

	ep->address = xe_mmio_read32(tile->primary_gt, CLIENT_DISC_ADDRESS);

	id = pci_dev_id(to_pci_dev(tile->xe->drm.dev));
	snprintf(tile->amc.i2c_name, sizeof(tile->amc.i2c_name), "amc_i2c-%x", id);

	sw_node->name = tile->amc.i2c_name;
	sw_node->properties = amc_i2c_properties;

	ret = amc_register_clock(tile->xe->drm.dev, &tile->amc, id);
	if (ret) {
		drm_warn(&tile->xe->drm, "Failed to register amc clock: %d\n", ret);
		goto err;
	}

	ret = software_node_register(sw_node);
	if (ret) {
		drm_warn(&tile->xe->drm, "Failed to register sw node: %d\n", ret);
		goto err_unreg_clock;
	}

	i2c_regmap = devm_regmap_init(tile->xe->drm.dev, NULL, tile,
				      &i2c_regmap_config);
	if (IS_ERR(i2c_regmap)) {
		drm_err(&tile->xe->drm, "failed to init I2C regmap\n");
		goto err_unreg_sw_node;
	}

	info.parent = tile->xe->drm.dev;
	info.fwnode = software_node_fwnode(sw_node);
	info.name = "i2c_designware";
	info.id = id;

	/*
	 * Currennt HW will not have an interrupt (polled).  However the desigware
	 * platform code needs this defined.  Use this as a place holder, and
	 * revisit after design is a little more baked.
	 */
	info.res = &DEFINE_RES_IRQ((to_pci_dev(tile->xe->drm.dev)->irq));
	info.num_res = 1;

	i2c_dev = platform_device_register_full(&info);
	if (IS_ERR(i2c_dev)) {
		drm_warn(&tile->xe->drm, "Failed to register platform info: %ld\n",
			 PTR_ERR(i2c_dev));
		goto err_unreg_sw_node;
	}
	tile->amc.i2c_dev = i2c_dev;

	drm_info(&tile->xe->drm, "AMC available: capabilities: 0x%x address: 0x%x\n",
		 ep->discovery.capabilities, ep->address);

	return;

err_unreg_sw_node:
	software_node_unregister(sw_node);
err_unreg_clock:
	amc_unregister_clock(&tile->amc);
err:
	ep->discovery.cookie = 0;
}

void amc_i2c_remove(struct xe_device *xe)
{
	struct xe_tile *tile;
	u8 id;

	for_each_tile(tile, xe, id) {
		if (tile->amc.endpoint.discovery.cookie == AMC_I2C_COOKIE) {
			platform_device_unregister(tile->amc.i2c_dev);
			amc_unregister_clock(&tile->amc);
			software_node_unregister(&tile->amc.sw_node);
		}
	}
}
