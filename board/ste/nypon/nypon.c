// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Stephan Gerhold <stephan@gerhold.net>
 */
#include <common.h>
#include <env.h>
#include <init.h>
#include <stdlib.h>
#include <asm/gpio.h>
#include <asm/setup.h>
#include <asm/io.h>
#include <linux/bitops.h>

DECLARE_GLOBAL_DATA_PTR;

extern uint fw_machid;
extern struct tag *fw_atags;

static struct tag *fw_atags_copy = NULL;
static uint fw_atags_size;

struct prcc_clk_init {
	unsigned int base;
	u32 pcken;
	u32 kcken;
};

static struct prcc_clk_init clk_init[] = {
	{
		.base = 0x8012f000,
		/* CLK_P1_GPIOCTRL */
		.pcken = BIT(9),
	},
	{
		.base = 0x8011f000,
		/* CLK_P2_SDI4, CLK_P2_GPIOCTRL */
		.pcken = BIT(4) | BIT(11),
		/* CLK_SDI4 */
		.kcken = BIT(2),
	},
	{
		.base = 0x8000f000,
		/* CLK_P3_UART2, CLK_P3_GPIOCTRL */
		.pcken = BIT(6) | BIT(8),
		/* CLK_UART2 */
		.kcken = BIT(6),
	},
	{
		.base = 0xa03ff000,
		/* CLK_P5_USB, CLK_P5_GPIOCTRL */
		.pcken = BIT(0) | BIT(1),
	},
	{
		.base = 0xa03cf000,
		/* CLK_P6_MTU0 */
		.pcken = BIT(6),
	}
};

#define PRCC_PCKEN	0x00
#define PRCC_PCKDIS	0x04
#define PRCC_KCKEN	0x08
#define PRCC_KCKDIS	0x0C
#define PRCC_PCKSR	0x10
#define PRCC_PKCKSR	0x14

int dram_init(void)
{
	/* TODO: Consider parsing ATAG_MEM instead */
	gd->ram_size = get_ram_size(CONFIG_SYS_SDRAM_BASE, CONFIG_SYS_SDRAM_SIZE);
	return 0;
}

int board_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clk_init); ++i) {
		if (clk_init[i].pcken)
			writel(clk_init[i].pcken, clk_init[i].base + PRCC_PCKEN);
		if (clk_init[i].kcken)
			writel(clk_init[i].kcken, clk_init[i].base + PRCC_KCKEN);
	}
	
	gd->bd->bi_arch_number = fw_machid;
	gd->bd->bi_boot_params = (ulong) fw_atags;
	return 0;
}

struct gpio_keys {
	struct gpio_desc vol_up;
	struct gpio_desc vol_down;
};

static void request_gpio_key(int node, const char *name, struct gpio_desc *desc)
{
	int ret;

	if (node < 0)
		return;

	ret = gpio_request_by_name_nodev(offset_to_ofnode(node), "gpios", 0,
					 desc, GPIOD_IS_IN);
	if (ret) {
		printf("Failed to request %s GPIO: %d\n", name, ret);
	}
}

static void request_gpio_keys(const void *fdt, struct gpio_keys *keys)
{
	int offset;
	int vol_up_node = -FDT_ERR_NOTFOUND;
	int vol_down_node = -FDT_ERR_NOTFOUND;

	/* Look for volume-up and volume-down subnodes of gpio-keys */
	offset = fdt_node_offset_by_compatible(fdt, -1, "gpio-keys");
	while (offset != -FDT_ERR_NOTFOUND) {
		if (vol_up_node < 0)
			vol_up_node = fdt_subnode_offset(fdt, offset, "volume-up");
		if (vol_down_node < 0)
			vol_down_node = fdt_subnode_offset(fdt, offset, "volume-down");

		if (vol_up_node >= 0 && vol_down_node >= 0)
			break;

		offset = fdt_node_offset_by_compatible(fdt, offset, "gpio-keys");
	}

	request_gpio_key(vol_up_node, "volume-up", &keys->vol_up);
	request_gpio_key(vol_down_node, "volume-down", &keys->vol_down);
}

static void check_keys(const void *fdt)
{
	struct gpio_keys keys = {0};

	if (!fdt)
		return;

	/* Request gpio-keys from device tree */
	request_gpio_keys(fdt, &keys);

	/* Boot into recovery? */
	if (dm_gpio_get_value(&keys.vol_up) == 1) {
		env_set("bootcmd", "run recoverybootcmd");
	}

	/* Boot into fastboot? */
	if (dm_gpio_get_value(&keys.vol_down) == 1) {
		env_set("preboot", "setenv preboot; run fastbootcmd");
	}
}

/*
 * The downstream/vendor kernel (provided by Samsung) uses atags for booting.
 * It also requires an extremely long cmdline provided by the primary bootloader
 * that is not suitable for booting mainline.
 *
 * Since downstream is the only user of atags, we emulate the behavior of the
 * Samsung bootloader by generating only the initrd atag in u-boot, and copying
 * all other atags as-is from the primary bootloader.
 */
static inline bool skip_atag(u32 tag)
{
	return (tag == ATAG_NONE || tag == ATAG_CORE ||
		tag == ATAG_INITRD || tag == ATAG_INITRD2);
}

static void parse_serial(struct tag_serialnr *serialnr)
{
	char serial[17];

	if (env_get("serial#"))
		return;

	sprintf(serial, "%08x%08x", serialnr->high, serialnr->low);
	env_set("serial#", serial);
}

static void copy_atags(struct tag *tags)
{
	struct tag *t, *copy;

	if (tags->hdr.tag != ATAG_CORE) {
		printf("Invalid atags provided by primary bootloader: "
		       "tag 0x%x at 0x%px\n", tags->hdr.tag, tags);
		return;
	}

	fw_atags_size = 0;

	/* Calculate necessary size for tags we want to copy */
	for_each_tag(t, tags) {
		if (skip_atag(t->hdr.tag))
			continue;

		if (t->hdr.tag == ATAG_SERIAL)
			parse_serial(&t->u.serialnr);

		fw_atags_size += t->hdr.size << 2;
	}

	if (!fw_atags_size)
		return;  /* No tags to copy */

	fw_atags_copy = malloc(fw_atags_size);
	if (!fw_atags_copy)
		return;
	copy = fw_atags_copy;

	/* Copy tags */
	for_each_tag(t, tags) {
		if (skip_atag(t->hdr.tag))
			continue;

		memcpy(copy, t, t->hdr.size << 2);
		copy = tag_next(copy);
	}
}

int misc_init_r(void)
{
	check_keys(gd->fdt_blob);
	copy_atags(fw_atags);
	return 0;
}

void setup_board_tags(struct tag **in_params)
{
	u8 **bytes = (u8**) in_params;
	if (!fw_atags_copy)
		return;

	memcpy(*bytes, fw_atags_copy, fw_atags_size);
	*bytes += fw_atags_size;
}
