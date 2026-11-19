/* Generated from Google's pcie-exynos-zuma-rc-cal.c (soc/gs android-gs-akita-5.15-android15), channel 1 branches only. */
static void cal_pwrdn_clear(void)
{
	void *phy_base_regs = PHY, *udbg_base_regs = UDBG;
	u32 val;

		// External PLL gating
		val = readl(udbg_base_regs + 0xC700) | 1u;
		writel(val, udbg_base_regs + 0xC700);
		delay_us(100);
		writel(0x00, phy_base_regs + 0x000C);

		writel(0x55, phy_base_regs + 0x0928);
		writel(0x02, phy_base_regs + 0x0580);

		writel(0x00, phy_base_regs + 0x0208);

		writel(0x00, phy_base_regs + 0x1044);
		writel(0x00, phy_base_regs + 0x1048);
		writel(0x00, phy_base_regs + 0x104C);
		writel(0x00, phy_base_regs + 0x1050);
		writel(0x00, phy_base_regs + 0x185C);

		//XO CLK gating
		//val = readl(udbg_base_regs + 0xC828) & ~(0x1 << 17);
		//writel(val, udbg_base_regs + 0xC828);
		delay_us(10);
}

static void cal_phy_config(void)
{
	void *elbi_base_regs = ELBI, *udbg_base_regs = UDBG, *soc_base_regs = SOC, *phy_base_regs = PHY, *phy_pcs_base_regs = PCS;
	int num_lanes = 1, lock_cnt;
	u32 ext_pll_lock, pll_lock, cdr_lock, oc_done, val, i;

		/* I/A will be disabled in PCIe poweroff
		   if (exynos_pcie->use_ia && exynos_pcie->info->set_ia_code) {
		//IA disable
		exynos_ia_write(exynos_pcie, 0x0, 0x60);
		exynos_ia_write(exynos_pcie, 0x0, 0x0);
		exynos_ia_write(exynos_pcie, 0xD0000004, 0x100);
		exynos_ia_write(exynos_pcie, 0xB0020188, 0x108);
		delay_us(10);
		}
		 */

		writel(0x3, phy_base_regs + 0x032C);    //PHY input clock un-gating

		val = readl(udbg_base_regs + 0xC700) | (0x1 << 0);
		writel(val, udbg_base_regs + 0xC700);        //Release External PLL init

		val = readl(udbg_base_regs + 0xC700) & ~(0x1 << 1);
		writel(val, udbg_base_regs + 0xC700);        //Override External PLL RESETB

		/* check external pll lock */
		ext_pll_lock = reg_wait(udbg_base_regs,
							    0xC734, 2, &lock_cnt);
		

		/* soc_ctrl setting */
		val = readl(soc_base_regs + 0x4004) & ~(1 << 2);
		writel(val, soc_base_regs + 0x4004);    //Select PHY input clock. 1b'1 = TCXO, 1b'0 = External PLL clock

		val = readl(soc_base_regs + 0x4004) | (0x3 << 4);
		writel(val, soc_base_regs + 0x4004);    //Select PHY input clock. 2b'11 = External PLL

		/* device type setting */
		writel(0x4, elbi_base_regs + 0x0080);

		/* soft_pwr_rst */
		writel(0xF, elbi_base_regs + 0x3A4);
		writel(0xD, elbi_base_regs + 0x3A4);
		delay_us(10);
		writel(0xF, elbi_base_regs + 0x3A4);
		delay_us(10);

		/* pma rst assert*/
		writel(0x1, elbi_base_regs + 0x1404);
		writel(0x1, elbi_base_regs + 0x1408);
		writel(0x1, elbi_base_regs + 0x1400);
		writel(0x0, elbi_base_regs + 0x1404);
		writel(0x0, elbi_base_regs + 0x1408);
		writel(0x0, elbi_base_regs + 0x1400);

		/* slv pend check */
		writel(0x1, elbi_base_regs + PCIE_SLV_PEND_SEL_NAK);

		/* pma_setting */

		//Common
		writel(0x88, phy_base_regs + 0x0000);
		writel(0x66, phy_base_regs + 0x001C);
		writel(0x00, phy_base_regs + 0x01F4);
		writel(0x59, phy_base_regs + 0x0514);
		writel(0x11, phy_base_regs + 0x051C);
		writel(0x0E, phy_base_regs + 0x062C);
		writel(0x22, phy_base_regs + 0x0644);
		writel(0x03, phy_base_regs + 0x0688);
		writel(0x28, phy_base_regs + 0x06D4);
		writel(0x64, phy_base_regs + 0x0788);
		writel(0x64, phy_base_regs + 0x078C);
		writel(0x50, phy_base_regs + 0x0790);
		writel(0x50, phy_base_regs + 0x0794);
		writel(0x05, phy_base_regs + 0x0944);
		writel(0x05, phy_base_regs + 0x0948);
		writel(0x05, phy_base_regs + 0x094C);
		writel(0x05, phy_base_regs + 0x0950);

		/* REFCLK 38.4Mhz to External PLL path */
		writel(0x02, phy_base_regs + 0x0590);
		writel(0xB0, phy_base_regs + 0x07F8);
		writel(0x08, phy_base_regs + 0x0730);

		//Delay@L1.2 = 7.68us
		writel(0xC0, phy_base_regs + 0x344);

		//test with no relatin of CLKREQ @L1.2
		writel(0x04, phy_base_regs + 0x0040);
		writel(0x03, phy_base_regs + 0x0204);
		writel(0x1F, phy_base_regs + 0x02D4);
		writel(0x10, phy_base_regs + 0x0358);

		//ERIO on
		writel(0x01, phy_base_regs + 0x0018);

		//lane
		writel(0x5B, phy_base_regs + 0x0514);
		writel(0x0C, phy_base_regs + 0x0608);
		writel(0x0F, phy_base_regs + 0x060C);
		writel(0x0F, phy_base_regs + 0x0610);
		writel(0x0F, phy_base_regs + 0x0614);
		writel(0x0F, phy_base_regs + 0x0618);
		writel(0x80, phy_base_regs + 0x0510);
		writel(0x03, phy_base_regs + 0x0688);
		writel(0x23, phy_base_regs + 0x0644);
		writel(0x11, phy_base_regs + 0x0624);

		//PLL margin issue setting for ERIO (GEN1 & GEN2)
		writel(0x0f, phy_base_regs + 0x0630);
		writel(0x53, phy_base_regs + 0x06D0);

		for (i = 0; i < num_lanes; i++) {
			phy_base_regs += (i * 0x1000);

			writel(0x04, phy_base_regs + 0x1140);
			writel(0x04, phy_base_regs + 0x1144);
			writel(0x04, phy_base_regs + 0x1148);
			writel(0x02, phy_base_regs + 0x114C);
			writel(0x00, phy_base_regs + 0x1150);
			writel(0x00, phy_base_regs + 0x1154);
			writel(0x00, phy_base_regs + 0x1158);
			writel(0x00, phy_base_regs + 0x115C);
			writel(0x1C, phy_base_regs + 0x12CC);
			writel(0x6C, phy_base_regs + 0x12DC);
			writel(0x29, phy_base_regs + 0x130C);
			writel(0x2F, phy_base_regs + 0x13B4);
			writel(0x05, phy_base_regs + 0x1A64);
			writel(0x05, phy_base_regs + 0x1A68);
			writel(0x05, phy_base_regs + 0x1A84);
			writel(0x05, phy_base_regs + 0x1A88);
			writel(0x00, phy_base_regs + 0x1A98);
			writel(0x00, phy_base_regs + 0x1A9C);
			writel(0x07, phy_base_regs + 0x1AA8);
			writel(0x00, phy_base_regs + 0x1AB8);
			writel(0x00, phy_base_regs + 0x1ABC);
			writel(0x90, phy_base_regs + 0x1AF8);
			writel(0x03, phy_base_regs + 0x1B34);
			writel(0x03, phy_base_regs + 0x1BB0);
			writel(0x03, phy_base_regs + 0x1BB4);
			writel(0x06, phy_base_regs + 0x1BC0);
			writel(0x06, phy_base_regs + 0x1BC4);
			writel(0x01, phy_base_regs + 0x1BE8);
			writel(0x04, phy_base_regs + 0x1BF8);
			writel(0x00, phy_base_regs + 0x1C98);
			writel(0x81, phy_base_regs + 0x1CA4);
		}

		if (EP_TYPE == EP_BCM_WIFI) {
			/* L1SS exit link down issue in BCM */
			writel(0x02, phy_base_regs + 0x1B20);
			writel(0x23, phy_base_regs + 0x1340);
			writel(0x3C, phy_base_regs + 0x002C);
		} else if (EP_TYPE == EP_QC_WIFI) {
			/* TX over/under shoot improvement in QC */
			writel(0x1, phy_base_regs + 0x1818);
			writel(0x8, phy_base_regs + 0x1808);

			/* L1SS exit link down issue in QC */
			writel(0x2, phy_base_regs + 0x1B1C);
		}

		val = readl(phy_base_regs + 0x1350);
		val |= 0x1;
		writel(val, phy_base_regs + 0x1350);

		/* PCS setting */
		writel(0x100B0604, phy_pcs_base_regs + 0x190);//New Guide

		//test with no relatin of CLKREQ @L1.2
		writel(0x000700D5, phy_pcs_base_regs + 0x154);

		writel(0x16400000, phy_pcs_base_regs + 0x100);
		writel(0x08600000, phy_pcs_base_regs + 0x104);
		writel(0x18500000, phy_pcs_base_regs + 0x114);
		writel(0x60700000, phy_pcs_base_regs + 0x124);
		writel(0x00000007, phy_pcs_base_regs + 0x174);
		writel(0x00000100, phy_pcs_base_regs + 0x178);

		if (EP_TYPE == EP_BCM_WIFI) {
			writel(0x00000700, phy_pcs_base_regs + 0x17c);
		} else {
			writel(0x00000010, phy_pcs_base_regs + 0x17c);
		}

		/* Additional configuration for SAMSUNG WIFI */
		if (EP_TYPE == EP_SAMSUNG_WIFI) {
			phy_base_regs = PHY;

			//work around
			writel(0x3D, phy_pcs_base_regs + 0x8);// xo clock always on setting 1D -> 3D
			writel(0x02, phy_base_regs + 0x40);
			writel(0x02, phy_base_regs + 0x2D4);
			writel(0x02, phy_base_regs + 0x358);

			writel(0x02, phy_base_regs + 0x204);

			writel(0x700D5, phy_pcs_base_regs + 0x154);// always on clkref P1 CPM
			writel(0x16400000, phy_pcs_base_regs + 0x100);
			writel(0x08600000, phy_pcs_base_regs + 0x104);
			writel(0x18500000, phy_pcs_base_regs + 0x114);
			writel(0x60700000, phy_pcs_base_regs + 0x124);// P1cpm -> p1.2 timer3
			writel(0x5, phy_pcs_base_regs + 0x174);
			writel(0x100, phy_pcs_base_regs + 0x178);// timer 2 cnt
			writel(0x10, phy_pcs_base_regs + 0x17C);// timer 3 count
		}

		if (EP_TYPE == EP_BCM_WIFI) {
			writel(0x01600202, phy_pcs_base_regs + 0x110);
		}

		/* pma rst release */
		writel(0x1, elbi_base_regs + 0x1404);
		delay_us(10);
		writel(0x1, elbi_base_regs + 0x1408);
		writel(0x1, elbi_base_regs + 0x1400);

		/* check pll lock */
		pll_lock = reg_wait(phy_base_regs,
							0x0A80, 0, &lock_cnt);
		/* check cdr lock */
		cdr_lock = reg_wait(phy_base_regs,
							0x15C0, 4, &lock_cnt);
		

		/* check offset calibration */
		oc_done = reg_wait(phy_base_regs,
						       0x140C, 7, &lock_cnt);

		

		writel(0x0, phy_base_regs + 0x032C);    //PHY input clock un-gating

		//L1 exit off by DBI
		writel(0x1, elbi_base_regs + 0x1078);
	pr("wifi: PHY: external PLL lock %x, PLL lock %x, CDR lock %x, offset calibration %x\n", ext_pll_lock, pll_lock, cdr_lock, oc_done);
}
