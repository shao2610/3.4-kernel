/* drivers/video/msm_fb/mdp.c
 *
 * MSM MDP Interface (used by framebuffer core)
 *
 * Copyright (c) 2007-2013, The Linux Foundation. All rights reserved.
 * Copyright (C) 2007 Google Incorporated
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/clk.h>
#include <mach/hardware.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <asm/system.h>
#include <asm/mach-types.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <mach/event_timer.h>
#include <mach/clk.h>
#include "mdp.h"
#include "msm_fb.h"
#ifdef CONFIG_FB_MSM_MDP40
#include "mdp4.h"
#endif
#include "mipi_dsi.h"

#ifdef CONFIG_HAS_EARLYSUSPEND
#undef CONFIG_HAS_EARLYSUSPEND
#endif

uint32 mdp4_extn_disp;

static struct clk *mdp_clk;
static struct clk *mdp_pclk;
static struct clk *mdp_lut_clk;

struct res_mmu_clk {
	char *mmu_clk_name;
	struct clk *mmu_clk;
};

static struct res_mmu_clk mdp_sec_mmu_clks[] = {
	{"mdp_iommu_clk"}, {"rot_iommu_clk"},
	{"vcodec_iommu0_clk"}, {"vcodec_iommu1_clk"},
	{"smmu_iface_clk"}
};

int mdp_rev;
int mdp_iommu_split_domain;
u32 mdp_max_clk = 266667000;

static struct platform_device *mdp_init_pdev;
static struct regulator *footswitch;
static unsigned int mdp_footswitch_on;

struct completion mdp_ppp_comp;
struct semaphore mdp_ppp_mutex;
struct semaphore mdp_pipe_ctrl_mutex;

unsigned long mdp_timer_duration = (HZ/20);   /* 50 msecond */

boolean mdp_ppp_waiting = FALSE;
uint32 mdp_tv_underflow_cnt;
uint32 mdp_lcdc_underflow_cnt;

boolean mdp_current_clk_on = FALSE;
boolean mdp_is_in_isr = FALSE;

struct vsync vsync_cntrl;

/*
 * legacy mdp_in_processing is only for DMA2-MDDI
 * this applies to DMA2 block only
 */
uint32 mdp_in_processing = FALSE;

#ifdef CONFIG_FB_MSM_MDP40
uint32 mdp_intr_mask = MDP4_ANY_INTR_MASK;
#else
uint32 mdp_intr_mask = MDP_ANY_INTR_MASK;
#endif

MDP_BLOCK_TYPE mdp_debug[MDP_MAX_BLOCK];

atomic_t mdp_block_power_cnt[MDP_MAX_BLOCK];

spinlock_t mdp_spin_lock;
struct workqueue_struct *mdp_dma_wq;	/*mdp dma wq */
struct workqueue_struct *mdp_vsync_wq;	/*mdp vsync wq */

struct workqueue_struct *mdp_hist_wq;	/*mdp histogram wq */
bool mdp_pp_initialized = FALSE;

static struct workqueue_struct *mdp_pipe_ctrl_wq; /* mdp mdp pipe ctrl wq */
static struct delayed_work mdp_pipe_ctrl_worker;

static boolean mdp_suspended = FALSE;
ulong mdp4_display_intf;
DEFINE_MUTEX(mdp_suspend_mutex);

#ifdef CONFIG_FB_MSM_MDP40
struct mdp_dma_data dma2_data;
struct mdp_dma_data dma_s_data;
struct mdp_dma_data dma_e_data;
#else
static struct mdp_dma_data dma2_data;
static struct mdp_dma_data dma_s_data;
#ifndef CONFIG_FB_MSM_MDP303
static struct mdp_dma_data dma_e_data;
#endif
#endif

#ifdef CONFIG_FB_MSM_WRITEBACK_MSM_PANEL
struct mdp_dma_data dma_wb_data;
#endif

static struct mdp_dma_data dma3_data;

extern ktime_t mdp_dma2_last_update_time;

extern uint32 mdp_dma2_update_time_in_usec;
extern int mdp_lcd_rd_cnt_offset_slow;
extern int mdp_lcd_rd_cnt_offset_fast;
extern int mdp_usec_diff_threshold;

extern int first_pixel_start_x;
extern int first_pixel_start_y;

#ifdef MSM_FB_ENABLE_DBGFS
struct dentry *mdp_dir;
#endif

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static int mdp_suspend(struct platform_device *pdev, pm_message_t state);
#else
#define mdp_suspend NULL
#endif

struct timeval mdp_dma2_timeval;
struct timeval mdp_ppp_timeval;

#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend early_suspend;
#endif

static u32 mdp_irq;

static uint32 mdp_prim_panel_type = NO_PANEL;
#ifndef CONFIG_FB_MSM_MDP22

#define MDP_HIST_LUT_SIZE (256)
struct list_head mdp_hist_lut_list;
DEFINE_MUTEX(mdp_hist_lut_list_mutex);
uint32_t last_lut[MDP_HIST_LUT_SIZE];

#define CM_LUT
#ifdef CM_LUT


/* From mako's arch/arm/mach-msm/lge/lge_qc_lcdc_luts.c
 * Copyright (c) 2011, LG Electronics. All rights reserved.
 *
 */

/* pixel order : RBG */
static uint32_t lcd_color_preset_lut[256] = {
	/* default linear qlut */
	0x00000000, 0x00010101, 0x00020202, 0x00030303,
	0x00040404, 0x00050505, 0x00060606, 0x00070707,
	0x00080808, 0x00090909, 0x000a0a0a, 0x000b0b0b,
	0x000c0c0c, 0x000d0d0d, 0x000e0e0e, 0x000f0f0f,
	0x00101010, 0x00111111, 0x00121212, 0x00131313,
	0x00141414, 0x00151515, 0x00161616, 0x00171717,
	0x00181818, 0x00191919, 0x001a1a1a, 0x001b1b1b,
	0x001c1c1c, 0x001d1d1d, 0x001e1e1e, 0x001f1f1f,
	0x00202020, 0x00212121, 0x00222222, 0x00232323,
	0x00242424, 0x00252525, 0x00262626, 0x00272727,
	0x00282828, 0x00292929, 0x002a2a2a, 0x002b2b2b,
	0x002c2c2c, 0x002d2d2d, 0x002e2e2e, 0x002f2f2f,
	0x00303030, 0x00313131, 0x00323232, 0x00333333,
	0x00343434, 0x00353535, 0x00363636, 0x00373737,
	0x00383838, 0x00393939, 0x003a3a3a, 0x003b3b3b,
	0x003c3c3c, 0x003d3d3d, 0x003e3e3e, 0x003f3f3f,
	0x00404040, 0x00414141, 0x00424242, 0x00434343,
	0x00444444, 0x00454545, 0x00464646, 0x00474747,
	0x00484848, 0x00494949, 0x004a4a4a, 0x004b4b4b,
	0x004c4c4c, 0x004d4d4d, 0x004e4e4e, 0x004f4f4f,
	0x00505050, 0x00515151, 0x00525252, 0x00535353,
	0x00545454, 0x00555555, 0x00565656, 0x00575757,
	0x00585858, 0x00595959, 0x005a5a5a, 0x005b5b5b,
	0x005c5c5c, 0x005d5d5d, 0x005e5e5e, 0x005f5f5f,
	0x00606060, 0x00616161, 0x00626262, 0x00636363,
	0x00646464, 0x00656565, 0x00666666, 0x00676767,
	0x00686868, 0x00696969, 0x006a6a6a, 0x006b6b6b,
	0x006c6c6c, 0x006d6d6d, 0x006e6e6e, 0x006f6f6f,
	0x00707070, 0x00717171, 0x00727272, 0x00737373,
	0x00747474, 0x00757575, 0x00767676, 0x00777777,
	0x00787878, 0x00797979, 0x007a7a7a, 0x007b7b7b,
	0x007c7c7c, 0x007d7d7d, 0x007e7e7e, 0x007f7f7f,
	0x00808080, 0x00818181, 0x00828282, 0x00838383,
	0x00848484, 0x00858585, 0x00868686, 0x00878787,
	0x00888888, 0x00898989, 0x008a8a8a, 0x008b8b8b,
	0x008c8c8c, 0x008d8d8d, 0x008e8e8e, 0x008f8f8f,
	0x00909090, 0x00919191, 0x00929292, 0x00939393,
	0x00949494, 0x00959595, 0x00969696, 0x00979797,
	0x00989898, 0x00999999, 0x009a9a9a, 0x009b9b9b,
	0x009c9c9c, 0x009d9d9d, 0x009e9e9e, 0x009f9f9f,
	0x00a0a0a0, 0x00a1a1a1, 0x00a2a2a2, 0x00a3a3a3,
	0x00a4a4a4, 0x00a5a5a5, 0x00a6a6a6, 0x00a7a7a7,
	0x00a8a8a8, 0x00a9a9a9, 0x00aaaaaa, 0x00ababab,
	0x00acacac, 0x00adadad, 0x00aeaeae, 0x00afafaf,
	0x00b0b0b0, 0x00b1b1b1, 0x00b2b2b2, 0x00b3b3b3,
	0x00b4b4b4, 0x00b5b5b5, 0x00b6b6b6, 0x00b7b7b7,
	0x00b8b8b8, 0x00b9b9b9, 0x00bababa, 0x00bbbbbb,
	0x00bcbcbc, 0x00bdbdbd, 0x00bebebe, 0x00bfbfbf,
	0x00c0c0c0, 0x00c1c1c1, 0x00c2c2c2, 0x00c3c3c3,
	0x00c4c4c4, 0x00c5c5c5, 0x00c6c6c6, 0x00c7c7c7,
	0x00c8c8c8, 0x00c9c9c9, 0x00cacaca, 0x00cbcbcb,
	0x00cccccc, 0x00cdcdcd, 0x00cecece, 0x00cfcfcf,
	0x00d0d0d0, 0x00d1d1d1, 0x00d2d2d2, 0x00d3d3d3,
	0x00d4d4d4, 0x00d5d5d5, 0x00d6d6d6, 0x00d7d7d7,
	0x00d8d8d8, 0x00d9d9d9, 0x00dadada, 0x00dbdbdb,
	0x00dcdcdc, 0x00dddddd, 0x00dedede, 0x00dfdfdf,
	0x00e0e0e0, 0x00e1e1e1, 0x00e2e2e2, 0x00e3e3e3,
	0x00e4e4e4, 0x00e5e5e5, 0x00e6e6e6, 0x00e7e7e7,
	0x00e8e8e8, 0x00e9e9e9, 0x00eaeaea, 0x00ebebeb,
	0x00ececec, 0x00ededed, 0x00eeeeee, 0x00efefef,
	0x00f0f0f0, 0x00f1f1f1, 0x00f2f2f2, 0x00f3f3f3,
	0x00f4f4f4, 0x00f5f5f5, 0x00f6f6f6, 0x00f7f7f7,
	0x00f8f8f8, 0x00f9f9f9, 0x00fafafa, 0x00fbfbfb,
	0x00fcfcfc, 0x00fdfdfd, 0x00fefefe, 0x00ffffff
};

#define R_MASK    0x00ff0000
#define G_MASK    0x000000ff
#define B_MASK    0x0000ff00
#define R_SHIFT   16
#define G_SHIFT   0
#define B_SHIFT   8
#define lut2r(lut) ((lut & R_MASK) >> R_SHIFT)
#define lut2g(lut) ((lut & G_MASK) >> G_SHIFT)
#define lut2b(lut) ((lut & B_MASK) >> B_SHIFT)

#define NUM_QLUT  256
#define MAX_KCAL_V (NUM_QLUT-1)
#define scaled_by_kcal(rgb, kcal) \
                (((((unsigned int)(rgb) * (unsigned int)(kcal)) << 16) / \
                (unsigned int)MAX_KCAL_V) >> 16)

static int kcal_r = 255;
static int kcal_g = 255;
static int kcal_b = 255;
int mdp_preset_lut_update_lcdc(struct fb_cmap *cmap, uint32_t *internal_lut);

static void kcal_tuning_apply() {
	struct fb_cmap cmap;

	cmap.start = 0;
	cmap.len = 256;
	cmap.transp = NULL;

	cmap.red = (uint16_t *)&(kcal_r);
	cmap.green = (uint16_t *)&(kcal_g);
	cmap.blue = (uint16_t *)&(kcal_b);

	mdp_preset_lut_update_lcdc(&cmap, lcd_color_preset_lut);
}

static ssize_t kcal_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	if (!count)
		return -EINVAL;


	sscanf(buf, "%d %d %d", &kcal_r, &kcal_g, &kcal_b);

	kcal_tuning_apply();

	return count;
}

static ssize_t kcal_show(struct device *dev, struct device_attribute *attr,
                                                                char *buf)
{
        return sprintf(buf, "%d %d %d\n", kcal_r, kcal_g, kcal_b);
}

static DEVICE_ATTR(kcal, 0644, kcal_show, kcal_store);

#endif
uint32_t mdp_block2base(uint32_t block)
{
	uint32_t base = 0x0;
	switch (block) {
	case MDP_BLOCK_DMA_P:
		base = 0x90000;
		break;
	case MDP_BLOCK_DMA_S:
		base = 0xA0000;
		break;
	case MDP_BLOCK_VG_1:
		base = 0x20000;
		break;
	case MDP_BLOCK_VG_2:
		base = 0x30000;
		break;
	case MDP_BLOCK_RGB_1:
		base = 0x40000;
		break;
	case MDP_BLOCK_RGB_2:
		base = 0x50000;
		break;
	case MDP_BLOCK_OVERLAY_0:
		base = 0x10000;
		break;
	case MDP_BLOCK_OVERLAY_1:
		base = 0x18000;
		break;
	case MDP_BLOCK_OVERLAY_2:
		base = (mdp_rev >= MDP_REV_43) ? 0x88000 : 0;
		break;
	default:
		break;
	}
	return base;
}

int mdp_enable_iommu_clocks(void)
{
	int ret = 0, i;
	for (i = 0; i < ARRAY_SIZE(mdp_sec_mmu_clks); i++) {
		mdp_sec_mmu_clks[i].mmu_clk = clk_get(&mdp_init_pdev->dev,
			mdp_sec_mmu_clks[i].mmu_clk_name);
		if (IS_ERR(mdp_sec_mmu_clks[i].mmu_clk)) {
			pr_err(" %s: Get failed for clk %s", __func__,
				   mdp_sec_mmu_clks[i].mmu_clk_name);
			ret = PTR_ERR(mdp_sec_mmu_clks[i].mmu_clk);
			break;
		}
		ret = clk_prepare_enable(mdp_sec_mmu_clks[i].mmu_clk);
		if (ret) {
			clk_put(mdp_sec_mmu_clks[i].mmu_clk);
			mdp_sec_mmu_clks[i].mmu_clk = NULL;
		}
	}
	if (ret) {
		for (i--; i >= 0; i--) {
			clk_disable_unprepare(mdp_sec_mmu_clks[i].mmu_clk);
			clk_put(mdp_sec_mmu_clks[i].mmu_clk);
			mdp_sec_mmu_clks[i].mmu_clk = NULL;
		}
	}
	return ret;
}

int mdp_disable_iommu_clocks(void)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(mdp_sec_mmu_clks); i++) {
		clk_disable_unprepare(mdp_sec_mmu_clks[i].mmu_clk);
		clk_put(mdp_sec_mmu_clks[i].mmu_clk);
		mdp_sec_mmu_clks[i].mmu_clk = NULL;
	}
	return 0;
}

static uint32_t mdp_pp_block2hist_lut(uint32_t block)
{
	uint32_t valid = 0;
	switch (block) {
	case MDP_BLOCK_DMA_P:
		valid = (mdp_rev >= MDP_REV_40) ? 1 : 0;
		break;
	case MDP_BLOCK_DMA_S:
		valid = (mdp_rev >= MDP_REV_40) ? 1 : 0;
		break;
	case MDP_BLOCK_VG_1:
		valid = (mdp_rev >= MDP_REV_40) ? 1 : 0;
		break;
	case MDP_BLOCK_VG_2:
		valid = (mdp_rev >= MDP_REV_40) ? 1 : 0;
		break;
	default:
		break;
	}
	return valid;
}

static void mdp_hist_lut_init_mgmt(struct mdp_hist_lut_mgmt *mgmt,
		uint32_t block)
{
	mutex_init(&mgmt->lock);
	mgmt->block = block;

	mutex_lock(&mdp_hist_lut_list_mutex);
	list_add(&mgmt->list, &mdp_hist_lut_list);
	mutex_unlock(&mdp_hist_lut_list_mutex);
}

static int mdp_hist_lut_destroy(void)
{
	struct mdp_hist_lut_mgmt *temp;
	struct list_head *pos, *q;

	mutex_lock(&mdp_hist_lut_list_mutex);
	list_for_each_safe(pos, q, &mdp_hist_lut_list) {
		temp = list_entry(pos, struct mdp_hist_lut_mgmt, list);
		list_del(pos);
		kfree(temp);
	}
	mutex_unlock(&mdp_hist_lut_list_mutex);
	return 0;
}

static int mdp_hist_lut_init(void)
{
	int i;
	struct mdp_hist_lut_mgmt *temp;

	if (mdp_pp_initialized)
		return -EEXIST;
	for (i = 0; i < MDP_HIST_LUT_SIZE; i++)
		last_lut[i] = i | (i << 8) | (i << 16);

	INIT_LIST_HEAD(&mdp_hist_lut_list);

	if (mdp_rev >= MDP_REV_30) {
		temp = kmalloc(sizeof(struct mdp_hist_lut_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit;
		mdp_hist_lut_init_mgmt(temp, MDP_BLOCK_DMA_P);
	}

	if (mdp_rev >= MDP_REV_40) {
		temp = kmalloc(sizeof(struct mdp_hist_lut_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit_list;
		mdp_hist_lut_init_mgmt(temp, MDP_BLOCK_VG_1);

		temp = kmalloc(sizeof(struct mdp_hist_lut_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit_list;
		mdp_hist_lut_init_mgmt(temp, MDP_BLOCK_VG_2);
	}

	if (mdp_rev > MDP_REV_42) {
		temp = kmalloc(sizeof(struct mdp_hist_lut_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit_list;
		mdp_hist_lut_init_mgmt(temp, MDP_BLOCK_DMA_S);
	}
	return 0;

exit_list:
	mdp_hist_lut_destroy();
exit:
	pr_err("Failed initializing histogram LUT memory\n");
	return -ENOMEM;
}

static int mdp_hist_lut_block2mgmt(uint32_t block,
		struct mdp_hist_lut_mgmt **mgmt)
{
	struct mdp_hist_lut_mgmt *temp, *output;
	int ret = 0;

	output = NULL;

	mutex_lock(&mdp_hist_lut_list_mutex);
	list_for_each_entry(temp, &mdp_hist_lut_list, list) {
		if (temp->block == block)
			output = temp;
	}
	mutex_unlock(&mdp_hist_lut_list_mutex);

	if (output == NULL)
		ret = -EINVAL;
	else
		*mgmt = output;

	return ret;
}

static int mdp_hist_lut_write_off(struct mdp_hist_lut_data *data,
		struct mdp_hist_lut_info *info, uint32_t offset)
{
	int i;
	uint32_t element[MDP_HIST_LUT_SIZE];
	uint32_t base = mdp_block2base(info->block);
	uint32_t sel = info->bank_sel;


	if (data->len != MDP_HIST_LUT_SIZE) {
		pr_err("%s: data->len != %d", __func__, MDP_HIST_LUT_SIZE);
		return -EINVAL;
	}

	if (copy_from_user(&element, data->data,
				MDP_HIST_LUT_SIZE * sizeof(uint32_t))) {
		pr_err("%s: Error copying histogram data", __func__);
		return -ENOMEM;
	}
	mdp_clk_ctrl(1);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	for (i = 0; i < MDP_HIST_LUT_SIZE; i++) {
		last_lut[i] = element[i];
		MDP_OUTP(MDP_BASE + base + offset + (0x400*(sel)) + (4*i),
				element[i]);
	}
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_clk_ctrl(0);

	return 0;
}

static int mdp_hist_lut_write(struct mdp_hist_lut_data *data,
						struct mdp_hist_lut_info *info)
{
	int ret = 0;

	if (data->block != info->block) {
		ret = -1;
		pr_err("%s, data/info mdp_block mismatch! %d != %d\n",
				__func__, data->block, info->block);
		goto error;
	}

	switch (data->block) {
	case MDP_BLOCK_VG_1:
	case MDP_BLOCK_VG_2:
		ret = mdp_hist_lut_write_off(data, info, 0x3400);
		break;
	case MDP_BLOCK_DMA_P:
	case MDP_BLOCK_DMA_S:
		ret = mdp_hist_lut_write_off(data, info, 0x4800);
		break;
	default:
		ret = -EINVAL;
		goto error;
	}

error:
	return ret;
}

#define MDP_HIST_LUT_VG_EN_MASK (0x20000)
#define MDP_HIST_LUT_VG_EN_SHIFT (17)
#define MDP_HIST_LUT_VG_EN_OFFSET (0x0058)
#define MDP_HIST_LUT_VG_SEL_OFFSET (0x0064)
static void mdp_hist_lut_commit_vg(struct mdp_hist_lut_info *info)
{
	uint32_t out_en, temp_en;
	uint32_t base = mdp_block2base(info->block);
	temp_en = (info->is_enabled) ? (1 << MDP_HIST_LUT_VG_EN_SHIFT) : 0x0;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	out_en = inpdw(MDP_BASE + base + MDP_HIST_LUT_VG_EN_OFFSET) &
						~MDP_HIST_LUT_VG_EN_MASK;
	MDP_OUTP(MDP_BASE + base + MDP_HIST_LUT_VG_EN_OFFSET, out_en | temp_en);

	if (info->has_sel_update)
		MDP_OUTP(MDP_BASE + base + MDP_HIST_LUT_VG_SEL_OFFSET,
								info->bank_sel);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

#define MDP_HIST_LUT_DMA_EN_MASK (0x7)
#define MDP_HIST_LUT_DMA_SEL_MASK (0x400)
#define MDP_HIST_LUT_DMA_SEL_SHIFT (10)
#define MDP_HIST_LUT_DMA_P_OFFSET (0x0070)
#define MDP_HIST_LUT_DMA_S_OFFSET (0x0028)
static void mdp_hist_lut_commit_dma(struct mdp_hist_lut_info *info)
{
	uint32_t out, temp, mask;
	uint32_t base = mdp_block2base(info->block);
	uint32_t offset = (info->block == MDP_BLOCK_DMA_P) ?
		MDP_HIST_LUT_DMA_P_OFFSET : MDP_HIST_LUT_DMA_S_OFFSET;

	mask = MDP_HIST_LUT_DMA_EN_MASK;
	temp = (info->is_enabled) ? 0x7 : 0x0;

	if (info->has_sel_update) {
		mask |= MDP_HIST_LUT_DMA_SEL_MASK;
		temp |=  ((info->bank_sel & 0x1) << MDP_HIST_LUT_DMA_SEL_SHIFT);
	}

	out = inpdw(MDP_BASE + base + offset) & ~mask;
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	MDP_OUTP(MDP_BASE + base + offset, out | temp);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

static void mdp_hist_lut_commit_info(struct mdp_hist_lut_info *info)
{
	switch (info->block) {
	case MDP_BLOCK_VG_1:
	case MDP_BLOCK_VG_2:
		mdp_hist_lut_commit_vg(info);
		break;
	case MDP_BLOCK_DMA_P:
	case MDP_BLOCK_DMA_S:
		mdp_hist_lut_commit_dma(info);
		break;
	default:
		goto error;
	}

error:
	return;
}

static void mdp_hist_lut_update_info(struct mdp_hist_lut_info *info, int ops)
{
	info->bank_sel = (ops & 0x8) >> 3;
	info->is_enabled = (ops & 0x1) ? TRUE : FALSE;
	info->has_sel_update = (ops & 0x10) ? TRUE : FALSE;
}

int mdp_hist_lut_config(struct mdp_hist_lut_data *data)
{
	struct mdp_hist_lut_mgmt *mgmt = NULL;
	struct mdp_hist_lut_info info;
	int ret = 0;

	if (!mdp_pp_block2hist_lut(data->block)) {
		ret = -ENOTTY;
		goto error;
	}

	ret = mdp_hist_lut_block2mgmt(data->block, &mgmt);
	if (ret)
		goto error;

	mutex_lock(&mgmt->lock);

	info.block = mgmt->block;

	mdp_hist_lut_update_info(&info, data->ops);

	switch ((data->ops & 0x6) >> 1) {
	case 0x1:
		pr_info("%s: histogram LUT read not supported\n", __func__);
		break;
	case 0x2:
		ret = mdp_hist_lut_write(data, &info);
		if (ret)
			goto error_lock;
		break;
	default:
		break;
	}

	mdp_hist_lut_commit_info(&info);

error_lock:
	mutex_unlock(&mgmt->lock);
error:
	return ret;
}

spinlock_t mdp_lut_push_lock;
static int mdp_lut_i;

static int mdp_lut_hw_update(struct fb_cmap *cmap)
{
	int i;
	u16 *c[3];
	u16 r, g, b;

	c[0] = cmap->green;
	c[1] = cmap->blue;
	c[2] = cmap->red;

	for (i = 0; i < cmap->len; i++) {
		if (copy_from_user(&r, cmap->red++, sizeof(r)) ||
		    copy_from_user(&g, cmap->green++, sizeof(g)) ||
		    copy_from_user(&b, cmap->blue++, sizeof(b)))
			return -EFAULT;

		last_lut[i] = ((g & 0xff) | ((b & 0xff) << 8) |
				((r & 0xff) << 16));
#ifdef CONFIG_FB_MSM_MDP40
		MDP_OUTP(MDP_BASE + 0x94800 +
#else
		MDP_OUTP(MDP_BASE + 0x93800 +
#endif
			(0x400*mdp_lut_i) + cmap->start*4 + i*4, last_lut[i]);
	}

	return 0;
}

#ifdef CM_LUT
int mdp_preset_lut_update_lcdc(struct fb_cmap *cmap, uint32_t *internal_lut)
{
        uint32_t out;
        int i;
        u16 r, g, b;

        mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
        mdp_clk_ctrl(1);

        for (i = 0; i < cmap->len; i++) {
                r = lut2r(internal_lut[i]);
                g = lut2g(internal_lut[i]);
                b = lut2b(internal_lut[i]);
                r = scaled_by_kcal(r, *(cmap->red));
                g = scaled_by_kcal(g, *(cmap->green));
                b = scaled_by_kcal(b, *(cmap->blue));
                MDP_OUTP(MDP_BASE + 0x94800 +
                        (0x400*mdp_lut_i) + cmap->start*4 + i*4,
                                ((g & 0xff) |
                                 ((b & 0xff) << 8) |
                                 ((r & 0xff) << 16)));
        }

        /*mask off non LUT select bits*/
        out = inpdw(MDP_BASE + 0x90070) & ~((0x1 << 10) | 0x7);
        MDP_OUTP(MDP_BASE + 0x90070, (mdp_lut_i << 10) | 0x7 | out);
        mdp_clk_ctrl(0);
        mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
        mdp_lut_i = (mdp_lut_i + 1)%2;
        return 0;
}
#endif

static int mdp_lut_push;
static int mdp_lut_push_i;
static int mdp_lut_resume_needed;

static void mdp_lut_status_restore(void)
{
	unsigned long flags;

	if (mdp_lut_resume_needed) {
		spin_lock_irqsave(&mdp_lut_push_lock, flags);
		mdp_lut_push = 1;
		spin_unlock_irqrestore(&mdp_lut_push_lock, flags);
	}
}

static void mdp_lut_status_backup(void)
{
	uint32_t status = inpdw(MDP_BASE + 0x90070) & 0x7;

	if (status)
		mdp_lut_resume_needed = 1;
	else
		mdp_lut_resume_needed = 0;
}

static int mdp_lut_update_nonlcdc(struct fb_info *info, struct fb_cmap *cmap)
{
	int ret;
	unsigned long flags;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	mdp_clk_ctrl(1);
	ret = mdp_lut_hw_update(cmap);
	mdp_clk_ctrl(0);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	if (ret)
		return ret;

	spin_lock_irqsave(&mdp_lut_push_lock, flags);
	mdp_lut_push = 1;
	mdp_lut_push_i = mdp_lut_i;
	spin_unlock_irqrestore(&mdp_lut_push_lock, flags);
	mdp_lut_i = (mdp_lut_i + 1)%2;

	return 0;
}

static int mdp_lut_update_lcdc(struct fb_info *info, struct fb_cmap *cmap)
{
	int ret;
	uint32_t out;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	mdp_clk_ctrl(1);
	ret = mdp_lut_hw_update(cmap);

	if (ret) {
		mdp_clk_ctrl(0);
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
		return ret;
	}

	/*mask off non LUT select bits*/
	out = inpdw(MDP_BASE + 0x90070);
	MDP_OUTP(MDP_BASE + 0x90070, (mdp_lut_i << 10) | 0x7 | out);
	mdp_clk_ctrl(0);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_lut_i = (mdp_lut_i + 1)%2;

	return 0;
}

static void mdp_lut_enable(void)
{
	uint32_t out;
	unsigned long flags;

	if (mdp_lut_push) {
		spin_lock_irqsave(&mdp_lut_push_lock, flags);
		mdp_lut_push = 0;
		out = inpdw(MDP_BASE + 0x90070) & ~((0x1 << 10) | 0x7);
		MDP_OUTP(MDP_BASE + 0x90070,
				(mdp_lut_push_i << 10) | 0x7 | out);
		spin_unlock_irqrestore(&mdp_lut_push_lock, flags);
	}
}

#define MDP_REV42_HIST_MAX_BIN 128
#define MDP_REV41_HIST_MAX_BIN 32

#define MDP_HIST_DATA32_R_OFF 0x0100
#define MDP_HIST_DATA32_G_OFF 0x0200
#define MDP_HIST_DATA32_B_OFF 0x0300

#define MDP_HIST_DATA128_R_OFF 0x0400
#define MDP_HIST_DATA128_G_OFF 0x0800
#define MDP_HIST_DATA128_B_OFF 0x0C00

#define MDP_HIST_DATA_LUMA_OFF 0x0200

#define MDP_HIST_EXTRA_DATA0_OFF 0x0028
#define MDP_HIST_EXTRA_DATA1_OFF 0x002C

struct mdp_hist_mgmt *mdp_hist_mgmt_array[MDP_HIST_MGMT_MAX];

void __mdp_histogram_kickoff(struct mdp_hist_mgmt *mgmt)
{
	char *mdp_hist_base = MDP_BASE + mgmt->base;
	if (mgmt->mdp_is_hist_data == TRUE) {
		MDP_OUTP(mdp_hist_base + 0x0004, mgmt->frame_cnt);
		MDP_OUTP(mdp_hist_base, 1);
	}
}

void __mdp_histogram_reset(struct mdp_hist_mgmt *mgmt)
{
	char *mdp_hist_base = MDP_BASE + mgmt->base;
	MDP_OUTP(mdp_hist_base + 0x000C, 1);
}

static void mdp_hist_read_work(struct work_struct *data);

static int mdp_hist_init_mgmt(struct mdp_hist_mgmt *mgmt, uint32_t block)
{
	uint32_t bins, extra, index, intr = 0, term = 0;
	init_completion(&mgmt->mdp_hist_comp);
	mutex_init(&mgmt->mdp_hist_mutex);
	mutex_init(&mgmt->mdp_do_hist_mutex);
	mgmt->block = block;
	mgmt->base = mdp_block2base(block);
	mgmt->mdp_is_hist_start = FALSE;
	mgmt->mdp_is_hist_data = FALSE;
	mgmt->mdp_is_hist_valid = FALSE;
	mgmt->mdp_is_hist_init = FALSE;
	mgmt->frame_cnt = 0;
	mgmt->bit_mask = 0;
	mgmt->num_bins = 0;
	switch (block) {
	case MDP_BLOCK_DMA_P:
		term = MDP_HISTOGRAM_TERM_DMA_P;
		intr = (mdp_rev >= MDP_REV_40) ? INTR_DMA_P_HISTOGRAM :
								MDP_HIST_DONE;
		bins = (mdp_rev >= MDP_REV_42) ? MDP_REV42_HIST_MAX_BIN :
			MDP_REV41_HIST_MAX_BIN;
		extra = 2;
		mgmt->base += (mdp_rev >= MDP_REV_40) ? 0x5000 : 0x4000;
		index = MDP_HIST_MGMT_DMA_P;
		break;
	case MDP_BLOCK_DMA_S:
		term = MDP_HISTOGRAM_TERM_DMA_S;
		intr = INTR_DMA_S_HISTOGRAM;
		bins = MDP_REV42_HIST_MAX_BIN;
		extra = 2;
		mgmt->base += 0x5000;
		index = MDP_HIST_MGMT_DMA_S;
		break;
	case MDP_BLOCK_VG_1:
		term = MDP_HISTOGRAM_TERM_VG_1;
		intr = INTR_VG1_HISTOGRAM;
		bins = MDP_REV42_HIST_MAX_BIN;
		extra = 1;
		mgmt->base += 0x6000;
		index = MDP_HIST_MGMT_VG_1;
		break;
	case MDP_BLOCK_VG_2:
		term = MDP_HISTOGRAM_TERM_VG_2;
		intr = INTR_VG2_HISTOGRAM;
		bins = MDP_REV42_HIST_MAX_BIN;
		extra = 1;
		mgmt->base += 0x6000;
		index = MDP_HIST_MGMT_VG_2;
		break;
	default:
		term = MDP_HISTOGRAM_TERM_DMA_P;
		intr = (mdp_rev >= MDP_REV_40) ? INTR_DMA_P_HISTOGRAM :
								MDP_HIST_DONE;
		bins = (mdp_rev >= MDP_REV_42) ? MDP_REV42_HIST_MAX_BIN :
			MDP_REV41_HIST_MAX_BIN;
		extra = 2;
		mgmt->base += (mdp_rev >= MDP_REV_40) ? 0x5000 : 0x4000;
		index = MDP_HIST_MGMT_DMA_P;
	}
	mgmt->irq_term = term;
	mgmt->intr = intr;

	mgmt->c0 = kmalloc(bins * sizeof(uint32_t), GFP_KERNEL);
	if (mgmt->c0 == NULL)
		goto error;

	mgmt->c1 = kmalloc(bins * sizeof(uint32_t), GFP_KERNEL);
	if (mgmt->c1 == NULL)
		goto error_1;

	mgmt->c2 = kmalloc(bins * sizeof(uint32_t), GFP_KERNEL);
	if (mgmt->c2 == NULL)
		goto error_2;

	mgmt->extra_info = kmalloc(extra * sizeof(uint32_t), GFP_KERNEL);
	if (mgmt->extra_info == NULL)
		goto error_extra;

	INIT_WORK(&mgmt->mdp_histogram_worker, mdp_hist_read_work);
	mgmt->hist = NULL;

	mdp_hist_mgmt_array[index] = mgmt;
	return 0;

error_extra:
	kfree(mgmt->c2);
error_2:
	kfree(mgmt->c1);
error_1:
	kfree(mgmt->c0);
error:
	return -ENOMEM;
}

static void mdp_hist_del_mgmt(struct mdp_hist_mgmt *mgmt)
{
	kfree(mgmt->extra_info);
	kfree(mgmt->c2);
	kfree(mgmt->c1);
	kfree(mgmt->c0);
}

static int mdp_histogram_destroy(void)
{
	struct mdp_hist_mgmt *temp;
	int i;

	for (i = 0; i < MDP_HIST_MGMT_MAX; i++) {
		temp = mdp_hist_mgmt_array[i];
		if (!temp)
			continue;
		mdp_hist_del_mgmt(temp);
		kfree(temp);
		mdp_hist_mgmt_array[i] = NULL;
	}
	return 0;
}

static int mdp_histogram_init(void)
{
	struct mdp_hist_mgmt *temp;
	int i, ret;

	if (mdp_pp_initialized)
		return -EEXIST;

	mdp_hist_wq = alloc_workqueue("mdp_hist_wq",
					WQ_NON_REENTRANT | WQ_UNBOUND, 0);

	for (i = 0; i < MDP_HIST_MGMT_MAX; i++)
		mdp_hist_mgmt_array[i] = NULL;

	if (mdp_rev >= MDP_REV_30) {
		temp = kmalloc(sizeof(struct mdp_hist_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit;
		ret = mdp_hist_init_mgmt(temp, MDP_BLOCK_DMA_P);
		if (ret) {
			kfree(temp);
			goto exit;
		}
	}

	if (mdp_rev >= MDP_REV_40) {
		temp = kmalloc(sizeof(struct mdp_hist_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit_list;
		ret = mdp_hist_init_mgmt(temp, MDP_BLOCK_VG_1);
		if (ret)
			goto exit_list;

		temp = kmalloc(sizeof(struct mdp_hist_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit_list;
		ret = mdp_hist_init_mgmt(temp, MDP_BLOCK_VG_2);
		if (ret)
			goto exit_list;
	}

	if (mdp_rev >= MDP_REV_42) {
		temp = kmalloc(sizeof(struct mdp_hist_mgmt), GFP_KERNEL);
		if (!temp)
			goto exit_list;
		ret = mdp_hist_init_mgmt(temp, MDP_BLOCK_DMA_S);
		if (ret)
			goto exit_list;
	}

	return 0;

exit_list:
	mdp_histogram_destroy();
exit:
	return -ENOMEM;
}

int mdp_histogram_block2mgmt(uint32_t block, struct mdp_hist_mgmt **mgmt)
{
	struct mdp_hist_mgmt *temp, *output;
	int i, ret = 0;

	output = NULL;

	for (i = 0; i < MDP_HIST_MGMT_MAX; i++) {
		temp = mdp_hist_mgmt_array[i];
		if (!temp)
			continue;

		if (temp->block == block) {
			output = temp;
			break;
		}
	}

	if (output == NULL)
		ret = -EINVAL;
	else
		*mgmt = output;

	return ret;
}

static int mdp_histogram_enable(struct mdp_hist_mgmt *mgmt)
{
	uint32_t base;
	unsigned long flag;
	if (mgmt->mdp_is_hist_data == TRUE) {
		pr_err("%s histogram already started\n", __func__);
		return -EINVAL;
	}

	mdp_clk_ctrl(1);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	base = (uint32_t) (MDP_BASE + mgmt->base);
	/*First make sure that device is not collecting histogram*/
	mgmt->mdp_is_hist_data = FALSE;
	mgmt->mdp_is_hist_valid = FALSE;
	mgmt->mdp_is_hist_init = FALSE;
	spin_lock_irqsave(&mdp_spin_lock, flag);
	outp32(MDP_INTR_CLEAR, mgmt->intr);
	mdp_intr_mask &= ~mgmt->intr;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	MDP_OUTP(base + 0x001C, 0);
	MDP_OUTP(base + 0x0018, INTR_HIST_DONE | INTR_HIST_RESET_SEQ_DONE);
	MDP_OUTP(base + 0x0024, 0);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	mutex_unlock(&mgmt->mdp_hist_mutex);
	cancel_work_sync(&mgmt->mdp_histogram_worker);
	mutex_lock(&mgmt->mdp_hist_mutex);

	/*Then initialize histogram*/
	INIT_COMPLETION(mgmt->mdp_hist_comp);

	spin_lock_irqsave(&mdp_spin_lock, flag);
	MDP_OUTP(base + 0x0018, INTR_HIST_DONE | INTR_HIST_RESET_SEQ_DONE);
	MDP_OUTP(base + 0x0010, 1);
	MDP_OUTP(base + 0x001C, INTR_HIST_DONE | INTR_HIST_RESET_SEQ_DONE);

	outp32(MDP_INTR_CLEAR, mgmt->intr);
	mdp_intr_mask |= mgmt->intr;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	mdp_enable_irq(mgmt->irq_term);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	MDP_OUTP(base + 0x0004, mgmt->frame_cnt);
	if (mgmt->block != MDP_BLOCK_VG_1 && mgmt->block != MDP_BLOCK_VG_2)
		MDP_OUTP(base + 0x0008, mgmt->bit_mask);
	mgmt->mdp_is_hist_data = TRUE;
	mgmt->mdp_is_hist_valid = TRUE;
	mgmt->mdp_is_hist_init = FALSE;
	__mdp_histogram_reset(mgmt);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_clk_ctrl(0);
	return 0;
}

static int mdp_histogram_disable(struct mdp_hist_mgmt *mgmt)
{
	uint32_t base, status;
	unsigned long flag;
	if (mgmt->mdp_is_hist_data == FALSE) {
		pr_err("%s histogram already stopped\n", __func__);
		return -EINVAL;
	}

	mgmt->mdp_is_hist_data = FALSE;
	mgmt->mdp_is_hist_valid = FALSE;
	mgmt->mdp_is_hist_init = FALSE;

	base = (uint32_t) (MDP_BASE + mgmt->base);

	mdp_clk_ctrl(1);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	spin_lock_irqsave(&mdp_spin_lock, flag);
	outp32(MDP_INTR_CLEAR, mgmt->intr);
	mdp_intr_mask &= ~mgmt->intr;
	outp32(MDP_INTR_ENABLE, mdp_intr_mask);
	mdp_disable_irq_nosync(mgmt->irq_term);
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	if (mdp_rev >= MDP_REV_42)
		MDP_OUTP(base + 0x0020, 1);
	status = inpdw(base + 0x001C);
	status &= ~(INTR_HIST_DONE | INTR_HIST_RESET_SEQ_DONE);
	MDP_OUTP(base + 0x001C, status);

	MDP_OUTP(base + 0x0018, INTR_HIST_DONE | INTR_HIST_RESET_SEQ_DONE);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_clk_ctrl(0);

	if (mgmt->hist != NULL) {
		mgmt->hist = NULL;
		complete(&mgmt->mdp_hist_comp);
	}

	return 0;
}

/*call when spanning mgmt_array only*/
int _mdp_histogram_ctrl(boolean en, struct mdp_hist_mgmt *mgmt)
{
	int ret = 0;

	mutex_lock(&mgmt->mdp_hist_mutex);
	if (mgmt->mdp_is_hist_start && !mgmt->mdp_is_hist_data && en)
		ret = mdp_histogram_enable(mgmt);
	else if (mgmt->mdp_is_hist_data && !en)
		ret = mdp_histogram_disable(mgmt);
	mutex_unlock(&mgmt->mdp_hist_mutex);

	if (en == false)
		cancel_work_sync(&mgmt->mdp_histogram_worker);

	return ret;
}

int mdp_histogram_ctrl(boolean en, uint32_t block)
{
	struct mdp_hist_mgmt *mgmt = NULL;
	int ret = 0;

	ret = mdp_histogram_block2mgmt(block, &mgmt);
	if (ret)
		goto error;

	ret = _mdp_histogram_ctrl(en, mgmt);
error:
	return ret;
}

int mdp_histogram_ctrl_all(boolean en)
{
	struct mdp_hist_mgmt *temp;
	int i, ret = 0, ret_temp = 0;

	for (i = 0; i < MDP_HIST_MGMT_MAX; i++) {
		temp = mdp_hist_mgmt_array[i];
		if (!temp)
			continue;

		ret_temp = _mdp_histogram_ctrl(en, temp);
		if (ret_temp)
			ret = ret_temp;
	}
	return ret;
}

int mdp_histogram_start(struct mdp_histogram_start_req *req)
{
	struct mdp_hist_mgmt *mgmt = NULL;
	int ret;

	ret = mdp_histogram_block2mgmt(req->block, &mgmt);
	if (ret) {
		ret = -ENOTTY;
		goto error;
	}

	mutex_lock(&mgmt->mdp_do_hist_mutex);
	mutex_lock(&mgmt->mdp_hist_mutex);
	if (mgmt->mdp_is_hist_start == TRUE) {
		pr_err("%s histogram already started\n", __func__);
		ret = -EPERM;
		goto error_lock;
	}

	mgmt->block = req->block;
	mgmt->frame_cnt = req->frame_cnt;
	mgmt->bit_mask = req->bit_mask;
	mgmt->num_bins = req->num_bins;
	mgmt->hist = NULL;

	ret = mdp_histogram_enable(mgmt);

	mgmt->mdp_is_hist_start = TRUE;

error_lock:
	mutex_unlock(&mgmt->mdp_hist_mutex);
	mutex_unlock(&mgmt->mdp_do_hist_mutex);
error:
	return ret;
}

int mdp_histogram_stop(struct fb_info *info, uint32_t block)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *) info->par;
	struct mdp_hist_mgmt *mgmt = NULL;
	int ret;

	ret = mdp_histogram_block2mgmt(block, &mgmt);
	if (ret) {
		ret = -ENOTTY;
		goto error;
	}

	mutex_lock(&mgmt->mdp_do_hist_mutex);
	mutex_lock(&mgmt->mdp_hist_mutex);
	if (mgmt->mdp_is_hist_start == FALSE) {
		pr_err("%s histogram already stopped\n", __func__);
		ret = -EPERM;
		goto error_lock;
	}

	mgmt->mdp_is_hist_start = FALSE;

	if (!mfd->panel_power_on) {
		if (mgmt->hist != NULL) {
			mgmt->hist = NULL;
			complete(&mgmt->mdp_hist_comp);
		}
		ret = -EINVAL;
		goto error_lock;
	}

	ret = mdp_histogram_disable(mgmt);

	mutex_unlock(&mgmt->mdp_hist_mutex);
	cancel_work_sync(&mgmt->mdp_histogram_worker);
	mutex_unlock(&mgmt->mdp_do_hist_mutex);
	return ret;

error_lock:
	mutex_unlock(&mgmt->mdp_hist_mutex);
	mutex_unlock(&mgmt->mdp_do_hist_mutex);
error:
	return ret;
}

/*call from within mdp_hist_mutex context*/
static int _mdp_histogram_read_dma_data(struct mdp_hist_mgmt *mgmt)
{
	char *mdp_hist_base;
	uint32_t r_data_offset, g_data_offset, b_data_offset;
	int i, ret = 0;

	mdp_hist_base = MDP_BASE + mgmt->base;

	r_data_offset = (32 == mgmt->num_bins) ? MDP_HIST_DATA32_R_OFF :
		MDP_HIST_DATA128_R_OFF;
	g_data_offset = (32 == mgmt->num_bins) ? MDP_HIST_DATA32_G_OFF :
		MDP_HIST_DATA128_G_OFF;
	b_data_offset = (32 == mgmt->num_bins) ? MDP_HIST_DATA32_B_OFF :
		MDP_HIST_DATA128_B_OFF;

	if (mgmt->c0 == NULL || mgmt->c1 == NULL || mgmt->c2 == NULL) {
		ret = -ENOMEM;
		goto hist_err;
	}

	if (!mgmt->hist) {
		pr_err("%s: mgmt->hist not set, mgmt->hist = 0x%08x",
		__func__, (uint32_t) mgmt->hist);
		return -EINVAL;
	}

	if (mgmt->hist->bin_cnt != mgmt->num_bins) {
		pr_err("%s, bins config = %d, bin requested = %d", __func__,
					mgmt->num_bins, mgmt->hist->bin_cnt);
		return -EINVAL;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	mdp_clk_ctrl(1);
	for (i = 0; i < mgmt->num_bins; i++) {
		mgmt->c0[i] = inpdw(mdp_hist_base + r_data_offset + (4*i));
		mgmt->c1[i] = inpdw(mdp_hist_base + g_data_offset + (4*i));
		mgmt->c2[i] = inpdw(mdp_hist_base + b_data_offset + (4*i));
	}

	if (mdp_rev >= MDP_REV_42) {
		if (mgmt->extra_info) {
			mgmt->extra_info[0] = inpdw(mdp_hist_base +
					MDP_HIST_EXTRA_DATA0_OFF);
			mgmt->extra_info[1] = inpdw(mdp_hist_base +
					MDP_HIST_EXTRA_DATA0_OFF + 4);
		} else
			ret = -ENOMEM;
	}
	mdp_clk_ctrl(0);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	if (!ret)
		return ret;

hist_err:
	pr_err("%s: invalid hist buffer\n", __func__);
	return ret;
}

/*call from within mdp_hist_mutex context*/
static int _mdp_histogram_read_vg_data(struct mdp_hist_mgmt *mgmt)
{
	char *mdp_hist_base;
	int i, ret = 0;

	mdp_hist_base = MDP_BASE + mgmt->base;

	if (mgmt->c0 == NULL) {
		ret = -ENOMEM;
		goto hist_err;
	}

	if (!mgmt->hist) {
		pr_err("%s: mgmt->hist not set", __func__);
		return -EINVAL;
	}

	if (mgmt->hist->bin_cnt != mgmt->num_bins) {
		pr_err("%s, bins config = %d, bin requested = %d", __func__,
					mgmt->num_bins, mgmt->hist->bin_cnt);
		return -EINVAL;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	mdp_clk_ctrl(1);
	for (i = 0; i < mgmt->num_bins; i++)
		mgmt->c0[i] = inpdw(mdp_hist_base + MDP_HIST_DATA_LUMA_OFF +
									(4*i));

	if (mdp_rev >= MDP_REV_42) {
		if (mgmt->extra_info) {
			mgmt->extra_info[0] = inpdw(mdp_hist_base +
						MDP_HIST_EXTRA_DATA0_OFF);
		} else
			ret = -ENOMEM;
	}
	mdp_clk_ctrl(0);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	if (!ret)
		return ret;

hist_err:
	pr_err("%s: invalid hist buffer\n", __func__);
	return ret;
}

static void mdp_hist_read_work(struct work_struct *data)
{
	struct mdp_hist_mgmt *mgmt = container_of(data, struct mdp_hist_mgmt,
							mdp_histogram_worker);
	int ret = 0;
	bool hist_ready;
	mutex_lock(&mgmt->mdp_hist_mutex);
	if (mgmt->mdp_is_hist_data == FALSE) {
		pr_debug("%s, Histogram disabled before read.\n", __func__);
		ret = -EINVAL;
		goto error;
	}

	if (mgmt->hist == NULL) {
		if ((mgmt->mdp_is_hist_init == TRUE) &&
			((!completion_done(&mgmt->mdp_hist_comp)) &&
			waitqueue_active(&mgmt->mdp_hist_comp.wait)))
			pr_err("mgmt->hist invalid NULL\n");
		ret = -EINVAL;
	}
	hist_ready = (mgmt->mdp_is_hist_init && mgmt->mdp_is_hist_valid);

	if (!ret && hist_ready) {
		switch (mgmt->block) {
		case MDP_BLOCK_DMA_P:
		case MDP_BLOCK_DMA_S:
			ret = _mdp_histogram_read_dma_data(mgmt);
			break;
		case MDP_BLOCK_VG_1:
		case MDP_BLOCK_VG_2:
			ret = _mdp_histogram_read_vg_data(mgmt);
			break;
		default:
			pr_err("%s, invalid MDP block = %d\n", __func__,
								mgmt->block);
			ret = -EINVAL;
			goto error;
		}
	}
	/*
	 * if read was triggered by an underrun or failed copying,
	 * don't wake up readers
	 */
	if (!ret && hist_ready) {
		mgmt->hist = NULL;
		if (waitqueue_active(&mgmt->mdp_hist_comp.wait))
			complete(&mgmt->mdp_hist_comp);
	}

	if (mgmt->mdp_is_hist_valid == FALSE)
			mgmt->mdp_is_hist_valid = TRUE;
	if (mgmt->mdp_is_hist_init == FALSE)
			mgmt->mdp_is_hist_init = TRUE;

	mdp_clk_ctrl(1);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	if (!ret && hist_ready)
		__mdp_histogram_kickoff(mgmt);
	else
		__mdp_histogram_reset(mgmt);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	mdp_clk_ctrl(0);

error:
	mutex_unlock(&mgmt->mdp_hist_mutex);
}

/*call from within mdp_hist_mutex*/
static int _mdp_copy_hist_data(struct mdp_histogram_data *hist,
						struct mdp_hist_mgmt *mgmt)
{
	int ret;

	if (hist->c0) {
		ret = copy_to_user(hist->c0, mgmt->c0,
		sizeof(uint32_t) * (hist->bin_cnt));
		if (ret)
			goto err;
	}
	if (hist->c1) {
		ret = copy_to_user(hist->c1, mgmt->c1,
		sizeof(uint32_t) * (hist->bin_cnt));
		if (ret)
			goto err;
	}
	if (hist->c2) {
		ret = copy_to_user(hist->c2, mgmt->c2,
		sizeof(uint32_t) * (hist->bin_cnt));
		if (ret)
			goto err;
	}
	if (hist->extra_info) {
		ret = copy_to_user(hist->extra_info, mgmt->extra_info,
		sizeof(uint32_t) * ((hist->block > MDP_BLOCK_VG_2) ? 2 : 1));
		if (ret)
			goto err;
	}
err:
	return ret;
}

#define MDP_HISTOGRAM_TIMEOUT_MS	84 /*5 Frames*/
static int mdp_do_histogram(struct fb_info *info,
					struct mdp_histogram_data *hist)
{
	struct mdp_hist_mgmt *mgmt = NULL;
	int ret = 0;
	unsigned long timeout = (MDP_HISTOGRAM_TIMEOUT_MS * HZ) / 1000;

	ret = mdp_histogram_block2mgmt(hist->block, &mgmt);
	if (ret) {
		pr_info("%s - %d", __func__, __LINE__);
		ret = -EINVAL;
		return ret;
	}

	mutex_lock(&mgmt->mdp_do_hist_mutex);
	if (!mgmt->frame_cnt || (mgmt->num_bins == 0)) {
		pr_info("%s - frame_cnt = %d, num_bins = %d", __func__,
		mgmt->frame_cnt, mgmt->num_bins);
		ret = -EINVAL;
		goto error;
}
	if ((mdp_rev <= MDP_REV_41 && hist->bin_cnt > MDP_REV41_HIST_MAX_BIN)
		|| (mdp_rev == MDP_REV_42 &&
				hist->bin_cnt > MDP_REV42_HIST_MAX_BIN)) {
		pr_info("%s - mdp_rev = %d, num_bins = %d", __func__, mdp_rev,
								hist->bin_cnt);
		ret = -EINVAL;
		goto error;
}
	mutex_lock(&mgmt->mdp_hist_mutex);
	if (!mgmt->mdp_is_hist_data) {
		pr_info("%s - hist_data = false!", __func__);
		ret = -EINVAL;
		goto error_lock;
	}

	if (!mgmt->mdp_is_hist_start) {
		pr_err("%s histogram not started\n", __func__);
		ret = -EPERM;
		goto error_lock;
	}

	if (mgmt->hist != NULL) {
		pr_err("%s; histogram attempted to be read twice\n", __func__);
		ret = -EPERM;
		goto error_lock;
	}
	INIT_COMPLETION(mgmt->mdp_hist_comp);
	mgmt->hist = hist;
	mutex_unlock(&mgmt->mdp_hist_mutex);

	ret = wait_for_completion_killable_timeout(&mgmt->mdp_hist_comp,
								timeout);
	if (ret <= 0) {
		if (!ret) {
			mgmt->hist = NULL;
			ret = -ETIMEDOUT;
			pr_debug("%s: bin collection timedout", __func__);
		} else {
			mgmt->hist = NULL;
			pr_debug("%s: bin collection interrupted", __func__);
		}
		goto error;
	}

	mutex_lock(&mgmt->mdp_hist_mutex);
	if (mgmt->mdp_is_hist_data && mgmt->mdp_is_hist_init)
		ret =  _mdp_copy_hist_data(hist, mgmt);
	else
		ret = -ENODATA;
error_lock:
	mutex_unlock(&mgmt->mdp_hist_mutex);
error:
	mutex_unlock(&mgmt->mdp_do_hist_mutex);
	return ret;
}
#endif

static void send_vsync_work(struct work_struct *work)
{
	char buf[64];
	char *envp[2];

	snprintf(buf, sizeof(buf), "VSYNC=%llu",
			ktime_to_ns(vsync_cntrl.vsync_time));
	envp[0] = buf;
	envp[1] = NULL;
	kobject_uevent_env(&(vsync_cntrl.dev->kobj), KOBJ_CHANGE, envp);
}

#ifdef CONFIG_FB_MSM_MDP303
/* vsync_isr_handler: Called from isr context*/
static void vsync_isr_handler(void)
{
	vsync_cntrl.vsync_time = ktime_get();
	schedule_work(&(vsync_cntrl.vsync_work));
}
#endif

/* Returns < 0 on error, 0 on timeout, or > 0 on successful wait */
int mdp_ppp_pipe_wait(void)
{
	int ret = 1;
	boolean wait;
	unsigned long flag;

	/* wait 5 seconds for the operation to complete before declaring
	the MDP hung */
	spin_lock_irqsave(&mdp_spin_lock, flag);
	wait = mdp_ppp_waiting;
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	if (wait == TRUE) {
		ret = wait_for_completion_interruptible_timeout(&mdp_ppp_comp,
								5 * HZ);
		if (!ret)
			printk(KERN_ERR "%s: Timed out waiting for the MDP.\n",
				__func__);
	}

	return ret;
}

#define MAX_VSYNC_GAP		4
#define DEFAULT_FRAME_RATE	60

u32 mdp_get_panel_framerate(struct msm_fb_data_type *mfd)
{
	u32 frame_rate = 0, pixel_rate = 0, total_pixel;
	struct msm_panel_info *panel_info = &mfd->panel_info;

	if ((panel_info->type == MIPI_VIDEO_PANEL ||
	     panel_info->type == MIPI_CMD_PANEL) &&
	    panel_info->mipi.frame_rate)
		frame_rate = panel_info->mipi.frame_rate;

	if (mfd->dest == DISPLAY_LCD) {
		if (panel_info->type == MDDI_PANEL && panel_info->mddi.is_type1)
			frame_rate = panel_info->lcd.refx100 / (100 * 2);
		else if (panel_info->type != MIPI_CMD_PANEL)
			frame_rate = panel_info->lcd.refx100 / 100;
	}
	pr_debug("%s type=%d frame_rate=%d\n", __func__,
		 panel_info->type, frame_rate);

	if (frame_rate)
		return frame_rate;

	pixel_rate =
		(panel_info->type == MIPI_CMD_PANEL ||
		 panel_info->type == MIPI_VIDEO_PANEL) ?
		panel_info->mipi.dsi_pclk_rate :
		panel_info->clk_rate;

	if (!pixel_rate)
		pr_warn("%s pixel rate is zero\n", __func__);

	total_pixel =
		(panel_info->lcdc.h_back_porch +
		 panel_info->lcdc.h_front_porch +
		 panel_info->lcdc.h_pulse_width +
		 panel_info->xres) *
		(panel_info->lcdc.v_back_porch +
		 panel_info->lcdc.v_front_porch +
		 panel_info->lcdc.v_pulse_width +
		 panel_info->yres);

	if (total_pixel)
		frame_rate = pixel_rate / total_pixel;
	else
		pr_warn("%s total pixels are zero\n", __func__);

	if (frame_rate == 0) {
		frame_rate = DEFAULT_FRAME_RATE;
		pr_debug("%s frame rate=%d is default\n", __func__, frame_rate);
	}
	pr_debug("%s frame rate=%d total_pixel=%d, pixel_rate=%d\n", __func__,
		frame_rate, total_pixel, pixel_rate);

	return frame_rate;
}

static int mdp_diff_to_next_vsync(ktime_t cur_time,
			ktime_t last_vsync, u32 vsync_period)
{
	int diff_from_last, diff_to_next;
	/*
	 * Get interval beween last vsync and current time
	 * Current time = CPU programming MDP for next Vsync
	 */
	diff_from_last =
		(ktime_to_us(ktime_sub(cur_time, last_vsync)));
	diff_from_last /= USEC_PER_MSEC;
	/*
	 * If the last Vsync occurred too long ago, skip programming
	 * the timer
	 */
	if (diff_from_last < (vsync_period * MAX_VSYNC_GAP)) {
		if (diff_from_last > vsync_period)
			diff_to_next =
				(diff_from_last - vsync_period) % vsync_period;
		else
			diff_to_next = vsync_period - diff_from_last;
	} else {
		/* mark it out of range */
		diff_to_next = vsync_period + 1;
	}
	return diff_to_next;
}

void mdp_update_pm(struct msm_fb_data_type *mfd, ktime_t pre_vsync)
{
	u32 vsync_period;
	int diff_to_next;
	ktime_t cur_time, wakeup_time;

	if (!mfd->cpu_pm_hdl)
		return;
	vsync_period = mfd->panel_info.frame_interval;
	cur_time = ktime_get();
	diff_to_next = mdp_diff_to_next_vsync(cur_time,
					      pre_vsync,
					      vsync_period);
	if (diff_to_next > vsync_period)
		return;
	wakeup_time = ktime_add_ns(cur_time, diff_to_next * NSEC_PER_MSEC);
	activate_event_timer(mfd->cpu_pm_hdl, wakeup_time);
}

static DEFINE_SPINLOCK(mdp_lock);
static int mdp_irq_mask;
static int mdp_irq_enabled;

/*
 * mdp_enable_irq: can not be called from isr
 */
void mdp_enable_irq(uint32 term)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&mdp_lock, irq_flags);
	if (mdp_irq_mask & term) {
		printk(KERN_ERR "%s: MDP IRQ term-0x%x is already set, mask=%x irq=%d\n",
				__func__, term, mdp_irq_mask, mdp_irq_enabled);
	} else {
		mdp_irq_mask |= term;
		if (mdp_irq_mask && !mdp_irq_enabled) {
			mdp_irq_enabled = 1;
			enable_irq(mdp_irq);
		}
	}
	spin_unlock_irqrestore(&mdp_lock, irq_flags);
}

/*
 * mdp_disable_irq: can not be called from isr
 */
void mdp_disable_irq(uint32 term)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&mdp_lock, irq_flags);
	if (!(mdp_irq_mask & term)) {
		printk(KERN_ERR "%s: MDP IRQ term-0x%x is NOT set, mask=%x irq=%d\n",
				__func__, term, mdp_irq_mask, mdp_irq_enabled);
	} else {
		mdp_irq_mask &= ~term;
		if (!mdp_irq_mask && mdp_irq_enabled) {
			mdp_irq_enabled = 0;
			disable_irq(mdp_irq);
		}
	}
	spin_unlock_irqrestore(&mdp_lock, irq_flags);
}

void mdp_disable_irq_nosync(uint32 term)
{
	spin_lock(&mdp_lock);
	if (!(mdp_irq_mask & term)) {
		printk(KERN_ERR "%s: MDP IRQ term-0x%x is NOT set, mask=%x irq=%d\n",
				__func__, term, mdp_irq_mask, mdp_irq_enabled);
	} else {
		mdp_irq_mask &= ~term;
		if (!mdp_irq_mask && mdp_irq_enabled) {
			mdp_irq_enabled = 0;
			disable_irq_nosync(mdp_irq);
		}
	}
	spin_unlock(&mdp_lock);
}

void mdp_pipe_kickoff(uint32 term, struct msm_fb_data_type *mfd)
{
	unsigned long flag;
	/* complete all the writes before starting */
	wmb();

	/* kick off PPP engine */
	if (term == MDP_PPP_TERM) {
		if (mdp_debug[MDP_PPP_BLOCK])
			jiffies_to_timeval(jiffies, &mdp_ppp_timeval);

		/* let's turn on PPP block */
		mdp_pipe_ctrl(MDP_PPP_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

		mdp_enable_irq(term);
		INIT_COMPLETION(mdp_ppp_comp);
		spin_lock_irqsave(&mdp_spin_lock, flag);
		mdp_ppp_waiting = TRUE;
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
		outpdw(MDP_BASE + 0x30, 0x1000);
		wait_for_completion_killable(&mdp_ppp_comp);
		mdp_disable_irq(term);

		if (mdp_debug[MDP_PPP_BLOCK]) {
			struct timeval now;

			jiffies_to_timeval(jiffies, &now);
			mdp_ppp_timeval.tv_usec =
			    now.tv_usec - mdp_ppp_timeval.tv_usec;
			MSM_FB_DEBUG("MDP-PPP: %d\n",
				    (int)mdp_ppp_timeval.tv_usec);
		}
	} else if (term == MDP_DMA2_TERM) {
		if (mdp_debug[MDP_DMA2_BLOCK]) {
			MSM_FB_DEBUG("MDP-DMA2: %d\n",
				    (int)mdp_dma2_timeval.tv_usec);
			jiffies_to_timeval(jiffies, &mdp_dma2_timeval);
		}
		/* DMA update timestamp */
		mdp_dma2_last_update_time = ktime_get_real();
		/* let's turn on DMA2 block */
#ifdef CONFIG_FB_MSM_MDP22
		outpdw(MDP_CMD_DEBUG_ACCESS_BASE + 0x0044, 0x0);/* start DMA */
#else
		mdp_lut_enable();

#ifdef CONFIG_FB_MSM_MDP40
		outpdw(MDP_BASE + 0x000c, 0x0);	/* start DMA */
#else
		outpdw(MDP_BASE + 0x0044, 0x0);	/* start DMA */

#ifdef CONFIG_FB_MSM_MDP303

#ifdef CONFIG_FB_MSM_MIPI_DSI
		mipi_dsi_cmd_mdp_start();
#endif

#endif

#endif
#endif
#ifdef CONFIG_FB_MSM_MDP40
	} else if (term == MDP_DMA_S_TERM) {
		mdp_pipe_ctrl(MDP_DMA_S_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x0010, 0x0);	/* start DMA */
	} else if (term == MDP_DMA_E_TERM) {
		mdp_pipe_ctrl(MDP_DMA_E_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x0014, 0x0);	/* start DMA */
	} else if (term == MDP_OVERLAY0_TERM) {
		mdp_pipe_ctrl(MDP_OVERLAY0_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x0004, 0);
	} else if (term == MDP_OVERLAY1_TERM) {
		mdp_pipe_ctrl(MDP_OVERLAY1_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x0008, 0);
	} else if (term == MDP_OVERLAY2_TERM) {
		mdp_pipe_ctrl(MDP_OVERLAY2_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x00D0, 0);
	}
#else
	} else if (term == MDP_DMA_S_TERM) {
		mdp_pipe_ctrl(MDP_DMA_S_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x0048, 0x0);	/* start DMA */
	} else if (term == MDP_DMA_E_TERM) {
		mdp_pipe_ctrl(MDP_DMA_E_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		outpdw(MDP_BASE + 0x004C, 0x0);
	}
#endif
}

static struct platform_device *pdev_list[MSM_FB_MAX_DEV_LIST];
static int pdev_list_cnt;

static void mdp_pipe_ctrl_workqueue_handler(struct work_struct *work)
{
	mdp_pipe_ctrl(MDP_MASTER_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

static int mdp_clk_rate;

#ifdef CONFIG_FB_MSM_NO_MDP_PIPE_CTRL

/*
 * mdp_clk_disable_unprepare(void) called from thread context
 */
static void mdp_clk_disable_unprepare(void)
{
	mb();
	vsync_clk_disable_unprepare();

	if (mdp_clk != NULL)
		clk_disable_unprepare(mdp_clk);

	if (mdp_pclk != NULL)
		clk_disable_unprepare(mdp_pclk);

	if (mdp_lut_clk != NULL)
		clk_disable_unprepare(mdp_lut_clk);
}

/*
 * mdp_clk_prepare_enable(void) called from thread context
 */
static void mdp_clk_prepare_enable(void)
{
	if (mdp_clk != NULL)
		clk_prepare_enable(mdp_clk);

	if (mdp_pclk != NULL)
		clk_prepare_enable(mdp_pclk);

	if (mdp_lut_clk != NULL)
		clk_prepare_enable(mdp_lut_clk);

	vsync_clk_prepare_enable();
}

/*
 * mdp_clk_ctrl: called from thread context
 */
void mdp_clk_ctrl(int on)
{
	static int mdp_clk_cnt;

	mutex_lock(&mdp_suspend_mutex);
	if (on) {
		if (mdp_clk_cnt == 0)
			mdp_clk_prepare_enable();
		mdp_clk_cnt++;
	} else {
		if (mdp_clk_cnt) {
			mdp_clk_cnt--;
			if (mdp_clk_cnt == 0)
				mdp_clk_disable_unprepare();
		}
	}
	pr_debug("%s: on=%d cnt=%d\n", __func__, on, mdp_clk_cnt);
	mutex_unlock(&mdp_suspend_mutex);
}



void mdp_pipe_ctrl(MDP_BLOCK_TYPE block, MDP_BLOCK_POWER_STATE state,
		   boolean isr)
{
	/* do nothing */
}
#else
void mdp_pipe_ctrl(MDP_BLOCK_TYPE block, MDP_BLOCK_POWER_STATE state,
		   boolean isr)
{
	boolean mdp_all_blocks_off = TRUE;
	int i;
	unsigned long flag;
	struct msm_fb_panel_data *pdata;

	/*
	 * It is assumed that if isr = TRUE then start = OFF
	 * if start = ON when isr = TRUE it could happen that the usercontext
	 * could turn off the clocks while the interrupt is updating the
	 * power to ON
	 */
	WARN_ON(isr == TRUE && state == MDP_BLOCK_POWER_ON);

	spin_lock_irqsave(&mdp_spin_lock, flag);
	if (MDP_BLOCK_POWER_ON == state) {
		atomic_inc(&mdp_block_power_cnt[block]);

		if (MDP_DMA2_BLOCK == block)
			mdp_in_processing = TRUE;
	} else {
		atomic_dec(&mdp_block_power_cnt[block]);

		if (atomic_read(&mdp_block_power_cnt[block]) < 0) {
			/*
			* Master has to serve a request to power off MDP always
			* It also has a timer to power off.  So, in case of
			* timer expires first and DMA2 finishes later,
			* master has to power off two times
			* There shouldn't be multiple power-off request for
			* other blocks
			*/
			if (block != MDP_MASTER_BLOCK) {
				MSM_FB_INFO("mdp_block_power_cnt[block=%d] \
				multiple power-off request\n", block);
			}
			atomic_set(&mdp_block_power_cnt[block], 0);
		}

		if (MDP_DMA2_BLOCK == block)
			mdp_in_processing = FALSE;
	}
	spin_unlock_irqrestore(&mdp_spin_lock, flag);

	/*
	 * If it's in isr, we send our request to workqueue.
	 * Otherwise, processing happens in the current context
	 */
	if (isr) {
		if (mdp_current_clk_on) {
			/* checking all blocks power state */
			for (i = 0; i < MDP_MAX_BLOCK; i++) {
				if (atomic_read(&mdp_block_power_cnt[i]) > 0) {
					mdp_all_blocks_off = FALSE;
					break;
				}
			}

			if (mdp_all_blocks_off) {
				/* send workqueue to turn off mdp power */
				queue_delayed_work(mdp_pipe_ctrl_wq,
						   &mdp_pipe_ctrl_worker,
						   mdp_timer_duration);
			}
		}
	} else {
		down(&mdp_pipe_ctrl_mutex);
		/* checking all blocks power state */
		for (i = 0; i < MDP_MAX_BLOCK; i++) {
			if (atomic_read(&mdp_block_power_cnt[i]) > 0) {
				mdp_all_blocks_off = FALSE;
				break;
			}
		}

		/*
		 * find out whether a delayable work item is currently
		 * pending
		 */

		if (delayed_work_pending(&mdp_pipe_ctrl_worker)) {
			/*
			 * try to cancel the current work if it fails to
			 * stop (which means del_timer can't delete it
			 * from the list, it's about to expire and run),
			 * we have to let it run. queue_delayed_work won't
			 * accept the next job which is same as
			 * queue_delayed_work(mdp_timer_duration = 0)
			 */
			cancel_delayed_work(&mdp_pipe_ctrl_worker);
		}

		if ((mdp_all_blocks_off) && (mdp_current_clk_on)) {
			mutex_lock(&mdp_suspend_mutex);
			if (block == MDP_MASTER_BLOCK || mdp_suspended) {
				mdp_current_clk_on = FALSE;
				mb();
				/* turn off MDP clks */
				mdp_vsync_clk_disable();
				for (i = 0; i < pdev_list_cnt; i++) {
					pdata = (struct msm_fb_panel_data *)
						pdev_list[i]->dev.platform_data;
					if (pdata && pdata->clk_func)
						pdata->clk_func(0);
				}
				if (mdp_clk != NULL) {
					mdp_clk_rate = clk_get_rate(mdp_clk);
					clk_disable_unprepare(mdp_clk);
					if (mdp_hw_revision <=
						MDP4_REVISION_V2_1 &&
						mdp_clk_rate > 122880000) {
						clk_set_rate(mdp_clk,
							 122880000);
					}
					MSM_FB_DEBUG("MDP CLK OFF\n");
				}
				if (mdp_pclk != NULL) {
					clk_disable_unprepare(mdp_pclk);
					MSM_FB_DEBUG("MDP PCLK OFF\n");
				}
				if (mdp_lut_clk != NULL)
					clk_disable_unprepare(mdp_lut_clk);
			} else {
				/* send workqueue to turn off mdp power */
				queue_delayed_work(mdp_pipe_ctrl_wq,
						   &mdp_pipe_ctrl_worker,
						   mdp_timer_duration);
			}
			mutex_unlock(&mdp_suspend_mutex);
		} else if ((!mdp_all_blocks_off) && (!mdp_current_clk_on)) {
			mdp_current_clk_on = TRUE;
			/* turn on MDP clks */
			for (i = 0; i < pdev_list_cnt; i++) {
				pdata = (struct msm_fb_panel_data *)
					pdev_list[i]->dev.platform_data;
				if (pdata && pdata->clk_func)
					pdata->clk_func(1);
			}
			if (mdp_clk != NULL) {
				if (mdp_hw_revision <=
					MDP4_REVISION_V2_1 &&
					mdp_clk_rate > 122880000) {
					clk_set_rate(mdp_clk,
						 mdp_clk_rate);
				}
				clk_prepare_enable(mdp_clk);
				MSM_FB_DEBUG("MDP CLK ON\n");
			}
			if (mdp_pclk != NULL) {
				clk_prepare_enable(mdp_pclk);
				MSM_FB_DEBUG("MDP PCLK ON\n");
			}
			if (mdp_lut_clk != NULL)
				clk_prepare_enable(mdp_lut_clk);
			mdp_vsync_clk_enable();
		}
		up(&mdp_pipe_ctrl_mutex);
	}
}

void mdp_clk_ctrl(int on)
{
	/* do nothing */
}
#endif

void mdp_histogram_handle_isr(struct mdp_hist_mgmt *mgmt)
{
	uint32 isr, mask;
	char *base_addr = MDP_BASE + mgmt->base;
	isr = inpdw(base_addr + MDP_HIST_INTR_STATUS_OFF);
	mask = inpdw(base_addr + MDP_HIST_INTR_ENABLE_OFF);
	outpdw(base_addr + MDP_HIST_INTR_CLEAR_OFF, isr);
	mb();
	isr &= mask;
	if (isr & INTR_HIST_RESET_SEQ_DONE)
		__mdp_histogram_kickoff(mgmt);
	else if (isr & INTR_HIST_DONE)
		queue_work(mdp_hist_wq, &mgmt->mdp_histogram_worker);
}

#ifndef CONFIG_FB_MSM_MDP40
irqreturn_t mdp_isr(int irq, void *ptr)
{
	uint32 mdp_interrupt = 0;
	struct mdp_dma_data *dma;
	unsigned long flag;
	struct mdp_hist_mgmt *mgmt = NULL;
	int i, ret;
	int vsync_isr;
	/* Ensure all the register write are complete */
	mb();

	mdp_is_in_isr = TRUE;

	mdp_interrupt = inp32(MDP_INTR_STATUS);
	outp32(MDP_INTR_CLEAR, mdp_interrupt);

	mdp_interrupt &= mdp_intr_mask;

	if (mdp_interrupt & TV_ENC_UNDERRUN) {
		mdp_interrupt &= ~(TV_ENC_UNDERRUN);
		mdp_tv_underflow_cnt++;
	}

	if (!mdp_interrupt)
		goto out;

	/*Primary Vsync interrupt*/
	if (mdp_interrupt & MDP_PRIM_RDPTR) {
		spin_lock_irqsave(&mdp_spin_lock, flag);
		vsync_isr = vsync_cntrl.vsync_irq_enabled;
		if (!vsync_isr) {
			mdp_intr_mask &= ~MDP_PRIM_RDPTR;
			outp32(MDP_INTR_ENABLE, mdp_intr_mask);
			mdp_disable_irq_nosync(MDP_VSYNC_TERM);
			vsync_cntrl.disabled_clocks = 1;
		} else {
			vsync_isr_handler();
		}
		spin_unlock_irqrestore(&mdp_spin_lock, flag);

		if (!vsync_isr)
			mdp_pipe_ctrl(MDP_CMD_BLOCK,
				MDP_BLOCK_POWER_OFF, TRUE);
	}

	/* DMA3 TV-Out Start */
	if (mdp_interrupt & TV_OUT_DMA3_START) {
		/* let's disable TV out interrupt */
		mdp_intr_mask &= ~TV_OUT_DMA3_START;
		outp32(MDP_INTR_ENABLE, mdp_intr_mask);

		dma = &dma3_data;
		if (dma->waiting) {
			dma->waiting = FALSE;
			complete(&dma->comp);
		}
	}

	if (mdp_rev >= MDP_REV_30) {
		/* Only DMA_P histogram exists for this MDP rev*/
		if (mdp_interrupt & MDP_HIST_DONE) {
			ret = mdp_histogram_block2mgmt(MDP_BLOCK_DMA_P, &mgmt);
			if (!ret)
				mdp_histogram_handle_isr(mgmt);
			outp32(MDP_INTR_CLEAR, MDP_HIST_DONE);
		}

		/* LCDC UnderFlow */
		if (mdp_interrupt & LCDC_UNDERFLOW) {
			mdp_lcdc_underflow_cnt++;
			/*when underflow happens HW resets all the histogram
			  registers that were set before so restore them back
			  to normal.*/
			for (i = 0; i < MDP_HIST_MGMT_MAX; i++) {
				mgmt = mdp_hist_mgmt_array[i];
				if (!mgmt)
					continue;
				mgmt->mdp_is_hist_valid = FALSE;
			}
		}

		/* LCDC Frame Start */
		if (mdp_interrupt & LCDC_FRAME_START) {
			dma = &dma2_data;
			spin_lock_irqsave(&mdp_spin_lock, flag);
			vsync_isr = vsync_cntrl.vsync_irq_enabled;
			/* let's disable LCDC interrupt */
			if (dma->waiting) {
				dma->waiting = FALSE;
				complete(&dma->comp);
			}

			if (!vsync_isr) {
				mdp_intr_mask &= ~LCDC_FRAME_START;
				outp32(MDP_INTR_ENABLE, mdp_intr_mask);
				mdp_disable_irq_nosync(MDP_VSYNC_TERM);
				vsync_cntrl.disabled_clocks = 1;
			} else {
				vsync_isr_handler();
			}
			spin_unlock_irqrestore(&mdp_spin_lock, flag);

			if (!vsync_isr)
				mdp_pipe_ctrl(MDP_CMD_BLOCK,
					MDP_BLOCK_POWER_OFF, TRUE);
		}

		/* DMA2 LCD-Out Complete */
		if (mdp_interrupt & MDP_DMA_S_DONE) {
			dma = &dma_s_data;
			dma->busy = FALSE;
			mdp_pipe_ctrl(MDP_DMA_S_BLOCK, MDP_BLOCK_POWER_OFF,
									TRUE);
			complete(&dma->comp);
		}

		/* DMA_E LCD-Out Complete */
		if (mdp_interrupt & MDP_DMA_E_DONE) {
			dma = &dma_s_data;
			dma->busy = FALSE;
			mdp_pipe_ctrl(MDP_DMA_E_BLOCK, MDP_BLOCK_POWER_OFF,
									TRUE);
			complete(&dma->comp);
		}
	}

	/* DMA2 LCD-Out Complete */
	if (mdp_interrupt & MDP_DMA_P_DONE) {
		struct timeval now;

		mdp_dma2_last_update_time = ktime_sub(ktime_get_real(),
			mdp_dma2_last_update_time);
		if (mdp_debug[MDP_DMA2_BLOCK]) {
			jiffies_to_timeval(jiffies, &now);
			mdp_dma2_timeval.tv_usec =
			    now.tv_usec - mdp_dma2_timeval.tv_usec;
		}
#ifndef CONFIG_FB_MSM_MDP303
		dma = &dma2_data;
		spin_lock_irqsave(&mdp_spin_lock, flag);
		dma->busy = FALSE;
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
		mdp_pipe_ctrl(MDP_DMA2_BLOCK, MDP_BLOCK_POWER_OFF, TRUE);
		complete(&dma->comp);
#else
		if (mdp_prim_panel_type == MIPI_CMD_PANEL) {
			dma = &dma2_data;
			spin_lock_irqsave(&mdp_spin_lock, flag);
			dma->busy = FALSE;
			spin_unlock_irqrestore(&mdp_spin_lock, flag);
			mdp_pipe_ctrl(MDP_DMA2_BLOCK, MDP_BLOCK_POWER_OFF,
				TRUE);
			complete(&dma->comp);
		}
#endif
	}

	/* PPP Complete */
	if (mdp_interrupt & MDP_PPP_DONE) {
#ifdef	CONFIG_FB_MSM_MDP31
		MDP_OUTP(MDP_BASE + 0x00100, 0xFFFF);
#endif
		mdp_pipe_ctrl(MDP_PPP_BLOCK, MDP_BLOCK_POWER_OFF, TRUE);
		spin_lock_irqsave(&mdp_spin_lock, flag);
		if (mdp_ppp_waiting) {
			mdp_ppp_waiting = FALSE;
			complete(&mdp_ppp_comp);
		}
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
	}

out:
mdp_is_in_isr = FALSE;

	return IRQ_HANDLED;
}
#endif

static void mdp_drv_init(void)
{
	int i;

	for (i = 0; i < MDP_MAX_BLOCK; i++) {
		mdp_debug[i] = 0;
	}

	/* initialize spin lock and workqueue */
	spin_lock_init(&mdp_spin_lock);
	spin_lock_init(&mdp_lut_push_lock);
	mdp_dma_wq = create_singlethread_workqueue("mdp_dma_wq");
	mdp_vsync_wq = create_singlethread_workqueue("mdp_vsync_wq");
	mdp_pipe_ctrl_wq = create_singlethread_workqueue("mdp_pipe_ctrl_wq");
	INIT_DELAYED_WORK(&mdp_pipe_ctrl_worker,
			  mdp_pipe_ctrl_workqueue_handler);

	/* initialize semaphore */
	init_completion(&mdp_ppp_comp);
	sema_init(&mdp_ppp_mutex, 1);
	sema_init(&mdp_pipe_ctrl_mutex, 1);

	dma2_data.busy = FALSE;
	dma2_data.dmap_busy = FALSE;
	dma2_data.waiting = FALSE;
	init_completion(&dma2_data.comp);
	init_completion(&dma2_data.dmap_comp);
	sema_init(&dma2_data.mutex, 1);
	mutex_init(&dma2_data.ov_mutex);

	dma3_data.busy = FALSE;
	dma3_data.waiting = FALSE;
	init_completion(&dma3_data.comp);
	sema_init(&dma3_data.mutex, 1);

	dma_s_data.busy = FALSE;
	dma_s_data.waiting = FALSE;
	init_completion(&dma_s_data.comp);
	sema_init(&dma_s_data.mutex, 1);

#ifndef CONFIG_FB_MSM_MDP303
	dma_e_data.busy = FALSE;
	dma_e_data.waiting = FALSE;
	init_completion(&dma_e_data.comp);
	mutex_init(&dma_e_data.ov_mutex);
#endif
#ifdef CONFIG_FB_MSM_WRITEBACK_MSM_PANEL
	dma_wb_data.busy = FALSE;
	dma_wb_data.waiting = FALSE;
	init_completion(&dma_wb_data.comp);
	mutex_init(&dma_wb_data.ov_mutex);
#endif

	/* initializing mdp power block counter to 0 */
	for (i = 0; i < MDP_MAX_BLOCK; i++) {
		atomic_set(&mdp_block_power_cnt[i], 0);
	}
	INIT_WORK(&(vsync_cntrl.vsync_work), send_vsync_work);
	vsync_cntrl.disabled_clocks = 1;
#ifdef MSM_FB_ENABLE_DBGFS
	{
		struct dentry *root;
		char sub_name[] = "mdp";

		root = msm_fb_get_debugfs_root();
		if (root != NULL) {
			mdp_dir = debugfs_create_dir(sub_name, root);

			if (mdp_dir) {
				msm_fb_debugfs_file_create(mdp_dir,
					"dma2_update_time_in_usec",
					(u32 *) &mdp_dma2_update_time_in_usec);
				msm_fb_debugfs_file_create(mdp_dir,
					"vs_rdcnt_slow",
					(u32 *) &mdp_lcd_rd_cnt_offset_slow);
				msm_fb_debugfs_file_create(mdp_dir,
					"vs_rdcnt_fast",
					(u32 *) &mdp_lcd_rd_cnt_offset_fast);
				msm_fb_debugfs_file_create(mdp_dir,
					"mdp_usec_diff_threshold",
					(u32 *) &mdp_usec_diff_threshold);
				msm_fb_debugfs_file_create(mdp_dir,
					"mdp_current_clk_on",
					(u32 *) &mdp_current_clk_on);
#ifdef CONFIG_FB_MSM_LCDC
				msm_fb_debugfs_file_create(mdp_dir,
					"lcdc_start_x",
					(u32 *) &first_pixel_start_x);
				msm_fb_debugfs_file_create(mdp_dir,
					"lcdc_start_y",
					(u32 *) &first_pixel_start_y);
#endif
			}
		}
	}
#endif
}

static int mdp_probe(struct platform_device *pdev);
static int mdp_remove(struct platform_device *pdev);

static int mdp_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: suspending...\n");
	return 0;
}

static int mdp_runtime_resume(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: resuming...\n");
	return 0;
}

static struct dev_pm_ops mdp_dev_pm_ops = {
	.runtime_suspend = mdp_runtime_suspend,
	.runtime_resume = mdp_runtime_resume,
};


static struct platform_driver mdp_driver = {
	.probe = mdp_probe,
	.remove = mdp_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = mdp_suspend,
	.resume = NULL,
#endif
	.shutdown = NULL,
	.driver = {
		/*
		 * Driver name must match the device name added in
		 * platform.c.
		 */
		.name = "mdp",
		.pm = &mdp_dev_pm_ops,
	},
};

static int mdp_fps_level_change(struct platform_device *pdev, u32 fps_level)
{
	int ret = 0;
	ret = panel_next_fps_level_change(pdev, fps_level);
	return ret;
}

static int mdp_off(struct platform_device *pdev)
{
	int ret = 0;
	struct msm_fb_data_type *mfd = platform_get_drvdata(pdev);

	pr_debug("%s:+\n", __func__);
	mdp_histogram_ctrl_all(FALSE);

	mdp_clk_ctrl(1);
	mdp_lut_status_backup();

	if (mfd->panel.type == MIPI_CMD_PANEL)
		mdp4_dsi_cmd_off(pdev);
	else if (mfd->panel.type == MIPI_VIDEO_PANEL)
		mdp4_dsi_video_off(pdev);
	else if (mfd->panel.type == HDMI_PANEL ||
			mfd->panel.type == LCDC_PANEL ||
			mfd->panel.type == LVDS_PANEL)
		mdp4_lcdc_off(pdev);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	ret = panel_next_off(pdev);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mdp_clk_ctrl(0);

	if (mdp_rev >= MDP_REV_41 && mfd->panel.type == MIPI_CMD_PANEL)
		mdp_dsi_cmd_overlay_suspend(mfd);
	pr_debug("%s:-\n", __func__);
	return ret;
}

static int mdp_on(struct platform_device *pdev)
{
	int ret = 0;
	struct msm_fb_data_type *mfd;
	int i;
	mfd = platform_get_drvdata(pdev);

	pr_debug("%s:+\n", __func__);

	if (mdp_rev >= MDP_REV_40) {
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		mdp_clk_ctrl(1);
		mdp4_hw_init();

		/* Initialize HistLUT to last LUT */
		for (i = 0; i < MDP_HIST_LUT_SIZE; i++) {
			MDP_OUTP(MDP_BASE + 0x94800 + i*4, last_lut[i]);
			MDP_OUTP(MDP_BASE + 0x94C00 + i*4, last_lut[i]);
		}

		mdp_lut_status_restore();
		outpdw(MDP_BASE + 0x0038, mdp4_display_intf);
		if (mfd->panel.type == MIPI_CMD_PANEL) {
			mdp_vsync_cfg_regs(mfd, FALSE);
			mdp4_dsi_cmd_on(pdev);
		} else if (mfd->panel.type == MIPI_VIDEO_PANEL) {
			mdp4_dsi_video_on(pdev);
		} else if (mfd->panel.type == HDMI_PANEL ||
				mfd->panel.type == LCDC_PANEL ||
				mfd->panel.type == LVDS_PANEL) {
			mdp4_lcdc_on(pdev);
		}

		mdp_clk_ctrl(0);
		mdp4_overlay_reset();
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	}

	if (mdp_rev == MDP_REV_303 && mfd->panel.type == MIPI_CMD_PANEL) {
		vsync_cntrl.dev = mfd->fbi->dev;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	ret = panel_next_on(pdev);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mdp_histogram_ctrl_all(TRUE);
	pr_debug("%s:-\n", __func__);

#ifdef CM_LUT
	kcal_tuning_apply();
#endif
	return ret;
}

static int mdp_resource_initialized;
static struct msm_panel_common_pdata *mdp_pdata;

uint32 mdp_hw_revision;

/*
 * mdp_hw_revision:
 * 0 == V1
 * 1 == V2
 * 2 == V2.1
 *
 */
void mdp_hw_version(void)
{
	char *cp;
	uint32 *hp;

	if (mdp_pdata == NULL)
		return;

	mdp_hw_revision = MDP4_REVISION_NONE;
	if (mdp_pdata->hw_revision_addr == 0)
		return;

	/* tlmmgpio2 shadow */
	cp = (char *)ioremap(mdp_pdata->hw_revision_addr, 0x16);

	if (cp == NULL)
		return;

	hp = (uint32 *)cp;	/* HW_REVISION_NUMBER */
	mdp_hw_revision = *hp;
	iounmap(cp);

	mdp_hw_revision >>= 28;	/* bit 31:28 */
	mdp_hw_revision &= 0x0f;

	MSM_FB_DEBUG("%s: mdp_hw_revision=%x\n",
				__func__, mdp_hw_revision);
}

#ifdef CONFIG_MSM_BUS_SCALING
static uint32_t mdp_bus_scale_handle;
int mdp_bus_scale_update_request(uint32_t index)
{
	if (!mdp_pdata && (!mdp_pdata->mdp_bus_scale_table
	     || index > (mdp_pdata->mdp_bus_scale_table->num_usecases - 1))) {
		printk(KERN_ERR "%s invalid table or index\n", __func__);
		return -EINVAL;
	}
	if (mdp_bus_scale_handle < 1) {
		printk(KERN_ERR "%s invalid bus handle\n", __func__);
		return -EINVAL;
	}
	return msm_bus_scale_client_update_request(mdp_bus_scale_handle,
							index);
}
#endif
DEFINE_MUTEX(mdp_clk_lock);
int mdp_set_core_clk(u32 rate)
{
	int ret = -EINVAL;
	if (mdp_clk)
		ret = clk_set_rate(mdp_clk, rate);
	if (ret)
		pr_err("%s unable to set mdp clk rate", __func__);
	else
		pr_debug("%s mdp clk rate to be set %d: actual rate %ld\n",
			__func__, rate, clk_get_rate(mdp_clk));
	return ret;
}

int mdp_clk_round_rate(u32 rate)
{
	return clk_round_rate(mdp_clk, rate);
}

unsigned long mdp_get_core_clk(void)
{
	unsigned long clk_rate = 0;
	if (mdp_clk) {
		mutex_lock(&mdp_clk_lock);
		clk_rate = clk_get_rate(mdp_clk);
		mutex_unlock(&mdp_clk_lock);
	}

	return clk_rate;
}

static int mdp_irq_clk_setup(struct platform_device *pdev,
	char cont_splashScreen)
{
	int ret;

#ifdef CONFIG_FB_MSM_MDP40
	ret = request_irq(mdp_irq, mdp4_isr, IRQF_DISABLED, "MDP", 0);
#else
	ret = request_irq(mdp_irq, mdp_isr, IRQF_DISABLED, "MDP", 0);
#endif
	if (ret) {
		printk(KERN_ERR "mdp request_irq() failed!\n");
		return ret;
	}
	disable_irq(mdp_irq);

	footswitch = regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(footswitch)) {
		footswitch = NULL;
	} else {
		regulator_enable(footswitch);
		mdp_footswitch_on = 1;
	}

	mdp_clk = clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(mdp_clk)) {
		ret = PTR_ERR(mdp_clk);
		printk(KERN_ERR "can't get mdp_clk error:%d!\n", ret);
		free_irq(mdp_irq, 0);
		return ret;
	}

	mdp_pclk = clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(mdp_pclk))
		mdp_pclk = NULL;

	if (mdp_rev >= MDP_REV_42) {
		mdp_lut_clk = clk_get(&pdev->dev, "lut_clk");
		if (IS_ERR(mdp_lut_clk)) {
			ret = PTR_ERR(mdp_lut_clk);
			pr_err("can't get mdp_clk error:%d!\n", ret);
			clk_put(mdp_clk);
			free_irq(mdp_irq, 0);
			return ret;
		}
	} else {
		mdp_lut_clk = NULL;
	}

#ifdef CONFIG_FB_MSM_MDP40

	if (mdp_pdata)
		mdp_max_clk = mdp_pdata->mdp_max_clk;
	else
		pr_err("%s cannot get mdp max clk!\n", __func__);

	if (!mdp_max_clk)
		pr_err("%s mdp max clk is zero!\n", __func__);

	if (cont_splashScreen)
		mdp_clk_rate = clk_get_rate(mdp_clk);
	else
		mdp_clk_rate = mdp_max_clk;

	mutex_lock(&mdp_clk_lock);
	clk_set_rate(mdp_clk, mdp_clk_rate);
	if (mdp_lut_clk != NULL)
		clk_set_rate(mdp_lut_clk, mdp_clk_rate);
	mutex_unlock(&mdp_clk_lock);

	MSM_FB_DEBUG("mdp_clk: mdp_clk=%d\n", (int)clk_get_rate(mdp_clk));
#endif
	return 0;
}

static int mdp_probe(struct platform_device *pdev)
{
	struct platform_device *msm_fb_dev = NULL;
	struct msm_fb_data_type *mfd;
	struct msm_fb_panel_data *pdata = NULL;
	int rc;
	resource_size_t  size ;
	unsigned long flag;
	u32 frame_rate;
#ifdef CONFIG_FB_MSM_MDP40
	int intf, if_no;
#endif
#if defined(CONFIG_FB_MSM_MIPI_DSI) && defined(CONFIG_FB_MSM_MDP40)
	struct mipi_panel_info *mipi;
#endif

	if ((pdev->id == 0) && (pdev->num_resources > 0)) {
		mdp_init_pdev = pdev;
		mdp_pdata = pdev->dev.platform_data;

		size =  resource_size(&pdev->resource[0]);
		msm_mdp_base = ioremap(pdev->resource[0].start, size);

		MSM_FB_DEBUG("MDP HW Base phy_Address = 0x%x virt = 0x%x\n",
			(int)pdev->resource[0].start, (int)msm_mdp_base);

		if (unlikely(!msm_mdp_base))
			return -ENOMEM;

		mdp_irq = platform_get_irq(pdev, 0);
		if (mdp_irq < 0) {
			pr_err("mdp: can not get mdp irq\n");
			return -ENOMEM;
		}

		mdp_rev = mdp_pdata->mdp_rev;

		mdp_iommu_split_domain = mdp_pdata->mdp_iommu_split_domain;

		rc = mdp_irq_clk_setup(pdev, mdp_pdata->cont_splash_enabled);

		if (rc)
			return rc;

		mdp_hw_version();

		/* initializing mdp hw */
#ifdef CONFIG_FB_MSM_MDP40
		if (!(mdp_pdata->cont_splash_enabled))
			mdp4_hw_init();
#else
		mdp_hw_init();
#endif

#ifdef CONFIG_FB_MSM_OVERLAY
		mdp_hw_cursor_init();
#endif
		mdp_resource_initialized = 1;
		return 0;
	}

	if (!mdp_resource_initialized)
		return -EPERM;

	mfd = platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	if (pdev_list_cnt >= MSM_FB_MAX_DEV_LIST)
		return -ENOMEM;

	msm_fb_dev = platform_device_alloc("msm_fb", pdev->id);
	if (!msm_fb_dev)
		return -ENOMEM;

	/* link to the latest pdev */
	mfd->pdev = msm_fb_dev;
	mfd->mdp_rev = mdp_rev;
	mfd->vsync_init = NULL;

	mfd->ov0_wb_buf = MDP_ALLOC(sizeof(struct mdp_buf_type));
	mfd->ov1_wb_buf = MDP_ALLOC(sizeof(struct mdp_buf_type));
	memset((void *)mfd->ov0_wb_buf, 0, sizeof(struct mdp_buf_type));
	memset((void *)mfd->ov1_wb_buf, 0, sizeof(struct mdp_buf_type));

	if (mdp_pdata) {
		mfd->ov0_wb_buf->size = mdp_pdata->ov0_wb_size;
		mfd->ov1_wb_buf->size = mdp_pdata->ov1_wb_size;
		mfd->mem_hid = mdp_pdata->mem_hid;
	} else {
		mfd->ov0_wb_buf->size = 0;
		mfd->ov1_wb_buf->size = 0;
		mfd->mem_hid = 0;
	}

	/* initialize Post Processing data*/
	mdp_hist_lut_init();
	mdp_histogram_init();
	mdp_pp_initialized = TRUE;

	/* add panel data */
	if (platform_device_add_data
	    (msm_fb_dev, pdev->dev.platform_data,
	     sizeof(struct msm_fb_panel_data))) {
		printk(KERN_ERR "mdp_probe: platform_device_add_data failed!\n");
		rc = -ENOMEM;
		goto mdp_probe_err;
	}
	/* data chain */
	pdata = msm_fb_dev->dev.platform_data;
	pdata->on = mdp_on;
	pdata->off = mdp_off;
	pdata->fps_level_change = mdp_fps_level_change;
	pdata->next = pdev;

	mdp_clk_ctrl(1);

	mdp_prim_panel_type = mfd->panel.type;
	switch (mfd->panel.type) {
	case EXT_MDDI_PANEL:
	case MDDI_PANEL:
	case EBI2_PANEL:
		INIT_WORK(&mfd->dma_update_worker,
			  mdp_lcd_update_workqueue_handler);
		INIT_WORK(&mfd->vsync_resync_worker,
			  mdp_vsync_resync_workqueue_handler);
		mfd->hw_refresh = FALSE;

		if (mfd->panel.type == EXT_MDDI_PANEL) {
			/* 15 fps -> 66 msec */
			mfd->refresh_timer_duration = (66 * HZ / 1000);
		} else {
			/* 24 fps -> 42 msec */
			mfd->refresh_timer_duration = (42 * HZ / 1000);
		}

#ifdef CONFIG_FB_MSM_MDP22
		mfd->dma_fnc = mdp_dma2_update;
		mfd->dma = &dma2_data;
#else
		if (mfd->panel_info.pdest == DISPLAY_1) {
#if defined(CONFIG_FB_MSM_OVERLAY) && defined(CONFIG_FB_MSM_MDDI)
			mfd->dma_fnc = mdp4_mddi_overlay;
			mfd->cursor_update = mdp4_mddi_overlay_cursor;
#else
			mfd->dma_fnc = mdp_dma2_update;
#endif
			mfd->dma = &dma2_data;
			mfd->lut_update = mdp_lut_update_nonlcdc;
			mfd->do_histogram = mdp_do_histogram;
			mfd->start_histogram = mdp_histogram_start;
			mfd->stop_histogram = mdp_histogram_stop;
		} else {
			mfd->dma_fnc = mdp_dma_s_update;
			mfd->dma = &dma_s_data;
		}
#endif
		if (mdp_pdata)
			mfd->vsync_gpio = mdp_pdata->gpio;
		else
			mfd->vsync_gpio = -1;

#ifdef CONFIG_FB_MSM_MDP40
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		spin_lock_irqsave(&mdp_spin_lock, flag);
		mdp_intr_mask |= INTR_OVERLAY0_DONE;
		if (mdp_hw_revision < MDP4_REVISION_V2_1) {
			/* dmas dmap switch */
			mdp_intr_mask |= INTR_DMA_S_DONE;
		}
		outp32(MDP_INTR_ENABLE, mdp_intr_mask);
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

		if (mfd->panel.type == EBI2_PANEL)
			intf = EBI2_INTF;
		else
			intf = MDDI_INTF;

		if (mfd->panel_info.pdest == DISPLAY_1)
			if_no = PRIMARY_INTF_SEL;
		else
			if_no = SECONDARY_INTF_SEL;

		mdp4_display_intf_sel(if_no, intf);
#endif
		mdp_config_vsync(mdp_init_pdev, mfd);
		break;

#ifdef CONFIG_FB_MSM_MIPI_DSI
	case MIPI_VIDEO_PANEL:
#ifndef CONFIG_FB_MSM_MDP303
		mipi = &mfd->panel_info.mipi;
		mdp4_dsi_vsync_init(0);
		mfd->hw_refresh = TRUE;
		mfd->dma_fnc = mdp4_dsi_video_overlay;
		mfd->lut_update = mdp_lut_update_lcdc;
		mfd->do_histogram = mdp_do_histogram;
		mfd->start_histogram = mdp_histogram_start;
		mfd->stop_histogram = mdp_histogram_stop;
		if (mfd->panel_info.pdest == DISPLAY_1) {
			if_no = PRIMARY_INTF_SEL;
			mfd->dma = &dma2_data;
		} else {
			if_no = EXTERNAL_INTF_SEL;
			mfd->dma = &dma_e_data;
		}
		mdp4_display_intf_sel(if_no, DSI_VIDEO_INTF);
#else
		pdata->on = mdp_dsi_video_on;
		pdata->off = mdp_dsi_video_off;
		mfd->hw_refresh = TRUE;
		mfd->dma_fnc = mdp_dsi_video_update;
		mfd->do_histogram = mdp_do_histogram;
		mfd->start_histogram = mdp_histogram_start;
		mfd->stop_histogram = mdp_histogram_stop;
		mfd->vsync_ctrl = mdp_dma_video_vsync_ctrl;
		if (mfd->panel_info.pdest == DISPLAY_1)
			mfd->dma = &dma2_data;
		else {
			printk(KERN_ERR "Invalid Selection of destination panel\n");
			rc = -ENODEV;
			mdp_clk_ctrl(0);
			goto mdp_probe_err;
		}

#endif
		if (mdp_rev >= MDP_REV_40)
			mfd->cursor_update = mdp_hw_cursor_sync_update;
		else
			mfd->cursor_update = mdp_hw_cursor_update;
		break;

	case MIPI_CMD_PANEL:
#ifndef CONFIG_FB_MSM_MDP303
		mfd->dma_fnc = mdp4_dsi_cmd_overlay;
		mipi = &mfd->panel_info.mipi;
		mdp4_dsi_rdptr_init(0);
		if (mfd->panel_info.pdest == DISPLAY_1) {
			if_no = PRIMARY_INTF_SEL;
			mfd->dma = &dma2_data;
		} else {
			if_no = SECONDARY_INTF_SEL;
			mfd->dma = &dma_s_data;
		}
		mfd->lut_update = mdp_lut_update_nonlcdc;
		mfd->do_histogram = mdp_do_histogram;
		mfd->start_histogram = mdp_histogram_start;
		mfd->stop_histogram = mdp_histogram_stop;
		mdp4_display_intf_sel(if_no, DSI_CMD_INTF);
#else
		mfd->dma_fnc = mdp_dma2_update;
		mfd->do_histogram = mdp_do_histogram;
		mfd->start_histogram = mdp_histogram_start;
		mfd->stop_histogram = mdp_histogram_stop;
		mfd->vsync_ctrl = mdp_dma_vsync_ctrl;
		if (mfd->panel_info.pdest == DISPLAY_1)
			mfd->dma = &dma2_data;
		else {
			printk(KERN_ERR "Invalid Selection of destination panel\n");
			rc = -ENODEV;
			mdp_clk_ctrl(0);
			goto mdp_probe_err;
		}
		INIT_WORK(&mfd->dma_update_worker,
			mdp_lcd_update_workqueue_handler);
#endif
		mdp_config_vsync(mdp_init_pdev, mfd);
		break;
#endif

#ifdef CONFIG_FB_MSM_DTV
	case DTV_PANEL:
		mdp4_dtv_vsync_init(0);
		pdata->on = mdp4_dtv_on;
		pdata->off = mdp4_dtv_off;
		mfd->hw_refresh = TRUE;
		mfd->cursor_update = mdp_hw_cursor_update;
		mfd->dma_fnc = mdp4_dtv_overlay;
		mfd->dma = &dma_e_data;
		mfd->do_histogram = mdp_do_histogram;
		mfd->start_histogram = mdp_histogram_start;
		mfd->stop_histogram = mdp_histogram_stop;
		mdp4_display_intf_sel(EXTERNAL_INTF_SEL, DTV_INTF);
		break;
#endif
	case HDMI_PANEL:
	case LCDC_PANEL:
	case LVDS_PANEL:
#ifdef CONFIG_FB_MSM_MDP303
		pdata->on = mdp_lcdc_on;
		pdata->off = mdp_lcdc_off;
#endif
		mfd->hw_refresh = TRUE;
#if	defined(CONFIG_FB_MSM_OVERLAY) && defined(CONFIG_FB_MSM_MDP40)
		mfd->cursor_update = mdp_hw_cursor_sync_update;
#else
		mfd->cursor_update = mdp_hw_cursor_update;
#endif
#ifndef CONFIG_FB_MSM_MDP22
		mfd->lut_update = mdp_lut_update_lcdc;
		mfd->do_histogram = mdp_do_histogram;
		mfd->start_histogram = mdp_histogram_start;
		mfd->stop_histogram = mdp_histogram_stop;
#endif
#ifdef CONFIG_FB_MSM_OVERLAY
		mfd->dma_fnc = mdp4_lcdc_overlay;
#else
		mfd->dma_fnc = mdp_lcdc_update;
#endif

#ifdef CONFIG_FB_MSM_MDP40
		mdp4_lcdc_vsync_init(0);
		if (mfd->panel.type == HDMI_PANEL) {
			mfd->dma = &dma_e_data;
			mdp4_display_intf_sel(EXTERNAL_INTF_SEL, LCDC_RGB_INTF);
		} else {
			mfd->dma = &dma2_data;
			mdp4_display_intf_sel(PRIMARY_INTF_SEL, LCDC_RGB_INTF);
		}
#else
		mfd->dma = &dma2_data;
		mfd->vsync_ctrl = mdp_dma_lcdc_vsync_ctrl;
		spin_lock_irqsave(&mdp_spin_lock, flag);
		mdp_intr_mask &= ~MDP_DMA_P_DONE;
		outp32(MDP_INTR_ENABLE, mdp_intr_mask);
		spin_unlock_irqrestore(&mdp_spin_lock, flag);
#endif
		break;

	case TV_PANEL:
#if defined(CONFIG_FB_MSM_OVERLAY) && defined(CONFIG_FB_MSM_TVOUT)
		pdata->on = mdp4_atv_on;
		pdata->off = mdp4_atv_off;
		mfd->dma_fnc = mdp4_atv_overlay;
		mfd->dma = &dma_e_data;
		mdp4_display_intf_sel(EXTERNAL_INTF_SEL, TV_INTF);
#else
		pdata->on = mdp_dma3_on;
		pdata->off = mdp_dma3_off;
		mfd->hw_refresh = TRUE;
		mfd->dma_fnc = mdp_dma3_update;
		mfd->dma = &dma3_data;
#endif
		break;

#ifdef CONFIG_FB_MSM_WRITEBACK_MSM_PANEL
	case WRITEBACK_PANEL:
		{
			unsigned int mdp_version;
			mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON,
						 FALSE);
			mdp_version = inpdw(MDP_BASE + 0x0);
			mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF,
						FALSE);
			mdp4_wfd_init(0);
			pdata->on = mdp4_overlay_writeback_on;
			pdata->off = mdp4_overlay_writeback_off;
			mfd->dma_fnc = mdp4_writeback_overlay;
			mfd->dma = &dma_wb_data;
			mdp4_display_intf_sel(EXTERNAL_INTF_SEL, DTV_INTF);
		}
		break;
#endif
	default:
		printk(KERN_ERR "mdp_probe: unknown device type!\n");
		rc = -ENODEV;
		mdp_clk_ctrl(0);
		goto mdp_probe_err;
	}

	if (mdp_rev >= MDP_REV_40) {
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		mdp4_display_intf = inpdw(MDP_BASE + 0x0038);
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	}

	frame_rate = mdp_get_panel_framerate(mfd);
	if (frame_rate) {
		mfd->panel_info.frame_interval = 1000 / frame_rate;
		mfd->cpu_pm_hdl = add_event_timer(NULL, (void *)mfd);
	}
	mdp_clk_ctrl(0);

#ifdef CONFIG_MSM_BUS_SCALING
	if (!mdp_bus_scale_handle && mdp_pdata &&
		mdp_pdata->mdp_bus_scale_table) {
		mdp_bus_scale_handle =
			msm_bus_scale_register_client(
					mdp_pdata->mdp_bus_scale_table);
		if (!mdp_bus_scale_handle) {
			printk(KERN_ERR "%s not able to get bus scale\n",
				__func__);
			return -ENOMEM;
		}
	}

	/* req bus bandwidth immediately */
	if (!(mfd->cont_splash_done))
		mdp_bus_scale_update_request(5);

#endif

	/* set driver data */
	platform_set_drvdata(msm_fb_dev, mfd);

	rc = platform_device_add(msm_fb_dev);
	if (rc) {
		goto mdp_probe_err;
	}

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	pdev_list[pdev_list_cnt++] = pdev;
	mdp4_extn_disp = 0;

#ifdef CM_LUT
	rc = device_create_file(&pdev->dev, &dev_attr_kcal);
#endif
	return 0;

      mdp_probe_err:
	platform_device_put(msm_fb_dev);
#ifdef CONFIG_MSM_BUS_SCALING
	if (mdp_pdata && mdp_pdata->mdp_bus_scale_table &&
		mdp_bus_scale_handle > 0)
		msm_bus_scale_unregister_client(mdp_bus_scale_handle);
#endif
	return rc;
}

void mdp_footswitch_ctrl(boolean on)
{
	mutex_lock(&mdp_suspend_mutex);
	if (!mdp_suspended || mdp4_extn_disp || !footswitch ||
		mdp_rev <= MDP_REV_41) {
		mutex_unlock(&mdp_suspend_mutex);
		return;
	}

	if (on && !mdp_footswitch_on) {
		pr_debug("Enable MDP FS\n");
		regulator_enable(footswitch);
		mdp_footswitch_on = 1;
	} else if (!on && mdp_footswitch_on) {
		pr_debug("Disable MDP FS\n");
		regulator_disable(footswitch);
		mdp_footswitch_on = 0;
	}

	mutex_unlock(&mdp_suspend_mutex);
}

#ifdef CONFIG_PM
static void mdp_suspend_sub(void)
{
	/* cancel pipe ctrl worker */
	cancel_delayed_work(&mdp_pipe_ctrl_worker);

	/* for workder can't be cancelled... */
	flush_workqueue(mdp_pipe_ctrl_wq);

	/* let's wait for PPP completion */
	while (atomic_read(&mdp_block_power_cnt[MDP_PPP_BLOCK]) > 0)
		cpu_relax();

	/* try to power down */
	mdp_pipe_ctrl(MDP_MASTER_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mutex_lock(&mdp_suspend_mutex);
	mdp_suspended = TRUE;
	mutex_unlock(&mdp_suspend_mutex);
}
#endif

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static int mdp_suspend(struct platform_device *pdev, pm_message_t state)
{
	if (pdev->id == 0) {
		mdp_suspend_sub();
		if (mdp_current_clk_on) {
			printk(KERN_WARNING"MDP suspend failed\n");
			return -EBUSY;
		}
	}

	return 0;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mdp_early_suspend(struct early_suspend *h)
{
	mdp_suspend_sub();
#ifdef CONFIG_FB_MSM_DTV
	mdp4_dtv_set_black_screen();
#endif
	mdp_footswitch_ctrl(FALSE);
}

static void mdp_early_resume(struct early_suspend *h)
{
	mdp_footswitch_ctrl(TRUE);
	mutex_lock(&mdp_suspend_mutex);
	mdp_suspended = FALSE;
	mutex_unlock(&mdp_suspend_mutex);
}
#endif

static int mdp_remove(struct platform_device *pdev)
{
	if (footswitch != NULL)
		regulator_put(footswitch);

	/*free post processing memory*/
	mdp_histogram_destroy();
	mdp_hist_lut_destroy();
	mdp_pp_initialized = FALSE;

	iounmap(msm_mdp_base);
	pm_runtime_disable(&pdev->dev);
#ifdef CONFIG_MSM_BUS_SCALING
	if (mdp_pdata && mdp_pdata->mdp_bus_scale_table &&
		mdp_bus_scale_handle > 0)
		msm_bus_scale_unregister_client(mdp_bus_scale_handle);
#endif
#ifdef CM_LUT
	device_remove_file(&pdev->dev, &dev_attr_kcal);
#endif
	return 0;
}

static int mdp_register_driver(void)
{
#ifdef CONFIG_HAS_EARLYSUSPEND
	early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1;
	early_suspend.suspend = mdp_early_suspend;
	early_suspend.resume = mdp_early_resume;
	register_early_suspend(&early_suspend);
#endif

	return platform_driver_register(&mdp_driver);
}

static int __init mdp_driver_init(void)
{
	int ret;

	mdp_drv_init();

	ret = mdp_register_driver();
	if (ret) {
		printk(KERN_ERR "mdp_register_driver() failed!\n");
		return ret;
	}

#if defined(CONFIG_DEBUG_FS)
	mdp_debugfs_init();
#endif

	return 0;

}

module_init(mdp_driver_init);