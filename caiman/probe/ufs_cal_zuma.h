/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UFS PHY/UniPro calibration tables for zuma/zumapro (Tensor G3/G4), transcribed from
 * google-modules/soc/gs drivers/ufs/zuma/ufs-cal.h (Copyright 2022 Samsung Electronics Co., Ltd.,
 * GPL-2.0-only; a copy is in caiman/reference/google/ufs/zuma/). Only the tables a cold start at
 * PWM-G1 needs: init_cfg_evt0 (identical to init_cfg_evt1) and post_init_cfg_evt0, with the
 * USE_38_4_MHZ choice the file makes. The "mib" column (monitoring only) is left out.
 *
 * Because of these tables, images built with PROBE_UFS are GPL-2.0 as a whole (see prebuilt/NOTICE);
 * the rest of the probe stays MIT.
 */
#ifndef UFS_CAL_ZUMA_H
#define UFS_CAL_ZUMA_H

enum cal_lyr {
	CAL_END = 0,
	CAL_PCS_COMN,		/* UniPro-mapped PCS register, once */
	CAL_PCS_RX,		/* per lane, through the AXI aux lane select (RX lanes 4..) */
	CAL_PCS_TX,		/* per lane (TX lanes 0..) */
	CAL_PCS_RX_RND,		/* per lane: round(1e9 / mclk) */
	CAL_PCS_TX_RND,
	CAL_PCS_RX_LR,		/* per lane: mclk ticks of 1000 us, as 3 bytes at +0, +4, +8 */
	CAL_PCS_TX_LR,		/* per lane: mclk ticks of 3200 us */
	CAL_UNIPRO,		/* UniPro register, once */
	CAL_UNIPRO_PRD,		/* UniPro register, once: 16e9 / mclk */
	CAL_UNIPRO_ADAPT,	/* UniPro register, once: PA adapt length fix-up */
	CAL_PMA_COMN,		/* PHY PMA common register, once */
	CAL_PMA_TRSV,		/* PHY PMA per-lane register (+0x800 per lane) */
	CAL_EMB_CAL_WAIT,	/* per lane: wait until (PMA lane reg & val) == val */
};

struct cal_cfg {
	u16 addr;
	u16 val;
	u8 lyr;
};

/* init_cfg_evt0[]: applied before DME_LINKSTARTUP (ufs_cal_pre_link) */
static const struct cal_cfg cal_pre_link[] = {
	{0x0044, 0x00, CAL_UNIPRO_PRD},

	{0x2800, 0x40, CAL_PCS_COMN},
	{0x2808, 0x22, CAL_PCS_COMN},		/* 38.4 MHz reference */
	{0x2048, 0x00, CAL_PCS_RX_RND},
	{0x22A8, 0x00, CAL_PCS_TX_RND},
	{0x22A4, 0x02, CAL_PCS_TX},
	{0x22AC, 0x00, CAL_PCS_TX_LR},
	{0x2044, 0x00, CAL_PCS_RX},
	{0x206C, 0x00, CAL_PCS_RX_LR},
	{0x20BC, 0x79, CAL_PCS_RX},

	{0x2210, 0x01, CAL_PCS_RX},
	{0x2010, 0x01, CAL_PCS_TX},
	{0x2094, 0xF6, CAL_PCS_RX},
	{0x21FC, 0x00, CAL_PCS_TX},
	{0x2800, 0x00, CAL_PCS_COMN},

	{0x3178, 0x0, CAL_UNIPRO},
	{0x5000, 0x0, CAL_UNIPRO},
	{0x5004, 0x1, CAL_UNIPRO},
	{0x6084, 0x1, CAL_UNIPRO},
	{0x6080, 0x1, CAL_UNIPRO},

	{0x140, 0x08, CAL_PMA_COMN},
	{0x014, 0x19, CAL_PMA_COMN},
	{0x02C, 0x44, CAL_PMA_COMN},
	{0x030, 0xC4, CAL_PMA_COMN},
	{0x034, 0xC3, CAL_PMA_COMN},
	{0x03C, 0x88, CAL_PMA_COMN},
	{0x058, 0x1A, CAL_PMA_COMN},
	{0x064, 0x04, CAL_PMA_COMN},
	{0x150, 0x88, CAL_PMA_COMN},
	{0x19C, 0x4C, CAL_PMA_COMN},
	{0x1A0, 0x4C, CAL_PMA_COMN},

	{0x804, 0x44, CAL_PMA_TRSV},
	{0x808, 0x44, CAL_PMA_TRSV},
	{0x80C, 0x00, CAL_PMA_TRSV},
	{0x810, 0x18, CAL_PMA_TRSV},
	{0x814, 0xC0, CAL_PMA_TRSV},
	{0x81C, 0x1C, CAL_PMA_TRSV},
	{0xBB0, 0x8C, CAL_PMA_TRSV},
	{0x9F0, 0xD0, CAL_PMA_TRSV},
	{0xA20, 0xFA, CAL_PMA_TRSV},
	{0xA24, 0x60, CAL_PMA_TRSV},
	{0x8D0, 0x30, CAL_PMA_TRSV},
	{0x8E4, 0x05, CAL_PMA_TRSV},
	{0x8F4, 0x05, CAL_PMA_TRSV},
	{0x934, 0x1A, CAL_PMA_TRSV},
	{0x938, 0x12, CAL_PMA_TRSV},
	{0x93C, 0x5E, CAL_PMA_TRSV},
	{0x964, 0x2A, CAL_PMA_TRSV},
	{0x980, 0x54, CAL_PMA_TRSV},
	{0x998, 0x54, CAL_PMA_TRSV},
	{0x9CC, 0x00, CAL_PMA_TRSV},
	{0x9D0, 0x00, CAL_PMA_TRSV},
	{0xAAC, 0x00, CAL_PMA_TRSV},
	{0xAB0, 0x02, CAL_PMA_TRSV},

	{0x140, 0x0C, CAL_PMA_COMN},
	{0x140, 0x00, CAL_PMA_COMN},
	{0xC74, 0x01, CAL_EMB_CAL_WAIT},

	{0, 0, CAL_END}
};

/* post_init_cfg_evt0[]: applied after DME_LINKSTARTUP (ufs_cal_post_link) */
static const struct cal_cfg cal_post_link[] = {
	{0x3348, 0x0, CAL_UNIPRO_ADAPT},
	{0x334C, 0x0, CAL_UNIPRO_ADAPT},
	{0x38A4, 0x01, CAL_UNIPRO},
	{0x3290, 0x3E8, CAL_UNIPRO},
	{0x38A4, 0x00, CAL_UNIPRO},

	{0, 0, CAL_END}
};

#endif
