/*
 * TI AM64x (AM6442) Cortex-A53 cluster machine, first pass.
 *
 * 2x Cortex-A53, GICv3, PSCI, DDR, MSRAM, main UART0-6 (16550). Every other
 * MAIN-domain block from TRM (SPRUIM2K) Table 2-1 is an unimplemented device
 * named "am64x.<block>" so "-d unimp" shows which blocks firmware touches.
 *
 * Aim is speed and programmability, not determinism or cycle accuracy.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/unimp.h"
#include "hw/core/sysbus.h"
#include "qobject/qlist.h"
#include "system/device_tree.h"
#include "system/hvf.h"
#include "system/system.h"
#include "system/tcg.h"
#include "target/arm/cpu.h"

#define AM64X_NUM_CPUS      2
#define AM64X_GICD_BASE     0x01800000
#define AM64X_GICR_BASE     0x01840000
#define AM64X_GICR_STRIDE   0x20000
#define AM64X_MSRAM_BASE    0x70000000
#define AM64X_MSRAM_SIZE    (2 * MiB)
#define AM64X_DDR_BASE      0x80000000
#define AM64X_DDR_MAX       (2 * GiB)
#define AM64X_NUM_IRQS      480 /* SPIs; num-irq = this + 32 */
#define AM64X_UART_BASE     0x02800000
#define AM64X_UART_STRIDE   0x10000
#define AM64X_UART_IRQ0     178 /* SPI number, INTID 210 */
#define AM64X_NUM_UARTS     7
#define AM64X_UART_CLK      48000000
#define AM64X_GTC_HZ        200000000 /* GTC clock; TCG only, HVF uses host */

typedef struct {
    const char *name;
    hwaddr base;
    hwaddr size;
} Am64xBlock;

/* Sorted largest first: later (smaller) blocks win where TRM ranges overlap. */
static const Am64xBlock am64x_blocks[] = {
    { "am64x.fss0_dat_reg0", 0x400000000, 0x100000000 },
    { "am64x.fss0_dat_reg3", 0x500000000, 0x100000000 },
    { "am64x.pcie0_pcie_rp_dat1", 0x600000000, 0x100000000 },
    { "am64x.debugss_wrap0_ext_apb_0", 0x730000000, 0x10000000 },
    { "am64x.debugss_wrap0_ext_apb_1", 0x770000000, 0x10000000 },
    { "am64x.gpmc0_gpmc_data", 0x050000000, 0x8000000 },
    { "am64x.fss0_dat_reg1", 0x060000000, 0x8000000 },
    { "am64x.pcie0_pcie_rp_dat0", 0x068000000, 0x8000000 },
    { "am64x.debugss_wrap0_resv2_0", 0x700004000, 0x2000000 },
    { "am64x.debugss_wrap0_resv2_1", 0x740004000, 0x2000000 },
    { "am64x.stm0_stimulus", 0x042000000, 0x1000000 },
    { "am64x.dmsc0_dmsc_fw_vbus_cfg", 0x045000000, 0x1000000 },
    { "am64x.debugss_wrap0_resv4_0", 0x720006000, 0x1000000 },
    { "am64x.debugss_wrap0_resv4_1", 0x760006000, 0x1000000 },
    { "am64x.pcie0_pcie_rp_core", 0x00d000000, 0x800000 },
    { "am64x.r5fss0_icache0", 0x074000000, 0x800000 },
    { "am64x.r5fss0_dcache0", 0x074800000, 0x800000 },
    { "am64x.r5fss0_icache1", 0x075000000, 0x800000 },
    { "am64x.r5fss0_dcache1", 0x075800000, 0x800000 },
    { "am64x.r5fss1_icache0", 0x076000000, 0x800000 },
    { "am64x.r5fss1_dcache0", 0x076800000, 0x800000 },
    { "am64x.r5fss1_icache1", 0x077000000, 0x800000 },
    { "am64x.r5fss1_dcache1", 0x077800000, 0x800000 },
    { "am64x.gicss0_gic_translater", 0x001000000, 0x400000 },
    { "am64x.dmass0_ringacc_rt", 0x049000000, 0x400000 },
    { "am64x.dmass0_pktdma_ringrt", 0x04b800000, 0x400000 },
    { "am64x.dmass0_ringacc_src_fifos", 0x04e000000, 0x400000 },
    { "am64x.cpsw0_cpsw3", 0x008000000, 0x200000 },
    { "am64x.gicss0_gic", 0x001800000, 0x100000 },
    { "am64x.dmass0_intaggr_intr", 0x048000000, 0x100000 },
    { "am64x.dmass0_intaggr_gcntrti", 0x04a000000, 0x100000 },
    { "am64x.dmass0_bcdma_ringrt", 0x04bc00000, 0x100000 },
    { "am64x.dmsc0_intaggr_intr", 0x044300000, 0x80000 },
    { "am64x.dmass0_sec_proxy_scfg", 0x04a400000, 0x80000 },
    { "am64x.dmass0_sec_proxy_rt", 0x04a600000, 0x80000 },
    { "am64x.dmass0_sec_proxy_src_target_data", 0x04d000000, 0x80000 },
    { "am64x.mcu_m4fss0_blazar_iram", 0x005000000, 0x40000 },
    { "am64x.timermgr0_timer_mgr_timers", 0x037000000, 0x40000 },
    { "am64x.rom0_rom", 0x041800000, 0x40000 },
    { "am64x.dmass0_ringacc_cfg", 0x049800000, 0x40000 },
    { "am64x.dmass0_pktdma_tchanrt", 0x04aa00000, 0x40000 },
    { "am64x.mcu_ctrl_mmr0_cfg0", 0x004500000, 0x20000 },
    { "am64x.ctrl_mmr0_cfg0", 0x043000000, 0x20000 },
    { "am64x.dmsc0_cbass_fw", 0x045080000, 0x20000 },
    { "am64x.main_sec_mmr0_cfg2", 0x045900000, 0x20000 },
    { "am64x.main_sec_mmr0_cfg0", 0x045a00000, 0x20000 },
    { "am64x.dmass0_intaggr_unmap", 0x048180000, 0x20000 },
    { "am64x.dmass0_pktdma_ring", 0x0485e0000, 0x20000 },
    { "am64x.dmass0_pktdma_rchanrt", 0x04a800000, 0x20000 },
    { "am64x.dmass0_bcdma_rchanrt", 0x04a820000, 0x20000 },
    { "am64x.dmass0_bcdma_tchanrt", 0x04aa40000, 0x20000 },
    { "am64x.dmass0_bcdma_bchanrt", 0x04c000000, 0x20000 },
    { "am64x.pll0_cfg", 0x000680000, 0x10000 },
    { "am64x.mcu_m4fss0_idram_slv_ram", 0x005040000, 0x10000 },
    { "am64x.serdes_10g0_wiz16b2m4ct", 0x00f000000, 0x10000 },
    { "am64x.pru_icssg0_ram_slv_ram", 0x030010000, 0x10000 },
    { "am64x.pru_icssg1_ram_slv_ram", 0x030090000, 0x10000 },
    { "am64x.sa2_ul0_cp_ace_pka_eip29t2", 0x040920000, 0x10000 },
    { "am64x.dmsc0_dmsc_sram0", 0x044040000, 0x10000 },
    { "am64x.dmsc0_dmsc_sram1", 0x044050000, 0x10000 },
    { "am64x.cbass0_isc", 0x045880000, 0x10000 },
    { "am64x.cbass0_qos", 0x045d80000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_romv8", 0x730000000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_dbg_cpu0", 0x730010000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_cti_cpu0", 0x730020000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_pmu_cpu0", 0x730030000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_etm_cpu0", 0x730040000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_dbg_cpu1", 0x730110000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_pmu_cpu1", 0x730120000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_etm_cpu1", 0x730130000, 0x10000 },
    { "am64x.compute_cluster0_apbaddr_cti_cpu1", 0x730140000, 0x10000 },
    { "am64x.padcfg_ctrl0_cfg0", 0x0000f0000, 0x8000 },
    { "am64x.mcu_padcfg_ctrl0_cfg0", 0x004080000, 0x8000 },
    { "am64x.ddr16ss0_emif_ctlcfg", 0x00f308000, 0x8000 },
    { "am64x.mcan0_msgmem_wrap_msgmem_vbp_ram", 0x020708000, 0x8000 },
    { "am64x.mcan1_msgmem_wrap_msgmem_vbp_ram", 0x020718000, 0x8000 },
    { "am64x.spinlock0_spinlock", 0x02a000000, 0x8000 },
    { "am64x.cbass0_fw", 0x045000000, 0x8000 },
    { "am64x.dmass0_bcdma_ring", 0x048600000, 0x8000 },
    { "am64x.gtc0_cfg1", 0x000a90000, 0x4000 },
    { "am64x.gtc0_cfg2", 0x000aa0000, 0x4000 },
    { "am64x.gtc0_cfg3", 0x000ab0000, 0x4000 },
    { "am64x.pru_icssg0_pr1_pdsp0_iram_ram", 0x030034000, 0x4000 },
    { "am64x.pru_icssg0_pr1_pdsp1_iram_ram", 0x030038000, 0x4000 },
    { "am64x.pru_icssg1_pr1_pdsp0_iram_ram", 0x0300b4000, 0x4000 },
    { "am64x.pru_icssg1_pr1_pdsp1_iram_ram", 0x0300b8000, 0x4000 },
    { "am64x.dmsc0_secmgr", 0x044234000, 0x4000 },
    { "am64x.dmass0_intaggr_imap", 0x048100000, 0x4000 },
    { "am64x.dmass0_pktdma_rflow", 0x048430000, 0x4000 },
    { "am64x.dmass0_pktdma_tchan", 0x0484a0000, 0x4000 },
    { "am64x.pru_icssg0_dram0_slv_ram", 0x030000000, 0x2000 },
    { "am64x.pru_icssg0_dram1_slv_ram", 0x030002000, 0x2000 },
    { "am64x.pru_icssg0_pr1_rtu0_pr1_rtu0_iram_ram", 0x030004000, 0x2000 },
    { "am64x.pru_icssg0_pr1_rtu1_pr1_rtu1_iram_ram", 0x030006000, 0x2000 },
    { "am64x.pru_icssg0_pr1_pdsp_tx0_iram_ram", 0x03000a000, 0x2000 },
    { "am64x.pru_icssg0_pr1_pdsp_tx1_iram_ram", 0x03000c000, 0x2000 },
    { "am64x.pru_icssg0_pr1_icss_intc_intc_slv", 0x030020000, 0x2000 },
    { "am64x.pru_icssg1_dram0_slv_ram", 0x030080000, 0x2000 },
    { "am64x.pru_icssg1_dram1_slv_ram", 0x030082000, 0x2000 },
    { "am64x.pru_icssg1_pr1_rtu0_pr1_rtu0_iram_ram", 0x030084000, 0x2000 },
    { "am64x.pru_icssg1_pr1_rtu1_pr1_rtu1_iram_ram", 0x030086000, 0x2000 },
    { "am64x.pru_icssg1_pr1_pdsp_tx0_iram_ram", 0x03008a000, 0x2000 },
    { "am64x.pru_icssg1_pr1_pdsp_tx1_iram_ram", 0x03008c000, 0x2000 },
    { "am64x.pru_icssg1_pr1_icss_intc_intc_slv", 0x0300a0000, 0x2000 },
    { "am64x.dmsc0_aes", 0x04423c000, 0x2000 },
    { "am64x.dmass0_intaggr_gcntcfg", 0x048220000, 0x2000 },
    { "am64x.dmass0_pktdma_cred", 0x048410000, 0x2000 },
    { "am64x.dmass0_bcdma_bchan", 0x048420000, 0x2000 },
    { "am64x.dmass0_bcdma_tchan", 0x0484a4000, 0x2000 },
    { "am64x.dmass0_pktdma_rchan", 0x0484c0000, 0x2000 },
    { "am64x.dmass0_bcdma_rchan", 0x0484c2000, 0x2000 },
    { "am64x.debugss0_ctset2_wrap_cfg_ctset2_cfg", 0x73c022000, 0x2000 },
    { "am64x.psc0_psc", 0x000400000, 0x1000 },
    { "am64x.esm0_cfg", 0x000420000, 0x1000 },
    { "am64x.mcu_psc0_psc", 0x004000000, 0x1000 },
    { "am64x.mcu_pll0_cfg", 0x004040000, 0x1000 },
    { "am64x.mcu_esm0_cfg", 0x004100000, 0x1000 },
    { "am64x.mcu_mcrc64_0_mcrc64", 0x004d00000, 0x1000 },
    { "am64x.mcu_m4fss0_rat", 0x005ff0000, 0x1000 },
    { "am64x.pcie0_core_vmap_ob_mmrs", 0x00f101000, 0x1000 },
    { "am64x.pcie0_core_pcie_intd_cfg_intd_cfg", 0x00f102000, 0x1000 },
    { "am64x.mmcsd1_mmc_ctlcfg", 0x00fa00000, 0x1000 },
    { "am64x.mmcsd0_mmc_ctlcfg", 0x00fa10000, 0x1000 },
    { "am64x.fss0_otfa_cfg", 0x00fc20000, 0x1000 },
    { "am64x.elm0", 0x025010000, 0x1000 },
    { "am64x.pru_icssg0_rat", 0x030008000, 0x1000 },
    { "am64x.pru_icssg0_rat", 0x030009000, 0x1000 },
    { "am64x.pru_icssg0_pr1_iep0_slv", 0x03002e000, 0x1000 },
    { "am64x.pru_icssg0_pr1_iep1_slv", 0x03002f000, 0x1000 },
    { "am64x.pru_icssg0_pr1_mii_rt_pr1_mii_rt_g_cfg_regs_g", 0x030033000, 0x1000 },
    { "am64x.pru_icssg1_rat", 0x030088000, 0x1000 },
    { "am64x.pru_icssg1_rat", 0x030089000, 0x1000 },
    { "am64x.pru_icssg1_pr1_iep0_slv", 0x0300ae000, 0x1000 },
    { "am64x.pru_icssg1_pr1_iep1_slv", 0x0300af000, 0x1000 },
    { "am64x.pru_icssg1_pr1_mii_rt_pr1_mii_rt_g_cfg_regs_g", 0x0300b3000, 0x1000 },
    { "am64x.sa2_ul0_sa_ul_primary", 0x040900000, 0x1000 },
    { "am64x.debugss0_sys", 0x041000000, 0x1000 },
    { "am64x.dmsc0_dmsc_rat_mmrs", 0x044200000, 0x1000 },
    { "am64x.dmsc0_sec", 0x044230000, 0x1000 },
    { "am64x.cbass_infra1_fw", 0x045008000, 0x1000 },
    { "am64x.dmsc0_cbass_isc", 0x045808000, 0x1000 },
    { "am64x.cbass_infra1_isc", 0x045820000, 0x1000 },
    { "am64x.cbass_infra1_qos", 0x045d20000, 0x1000 },
    { "am64x.dmass0_psilss_mmrs", 0x048140000, 0x1000 },
    { "am64x.dmass0_intaggr_mcast", 0x048210000, 0x1000 },
    { "am64x.dmass0_etlsw_mmrs", 0x048230000, 0x1000 },
    { "am64x.debugss_wrap0_rom_table_0_0", 0x700000000, 0x1000 },
    { "am64x.debugss_wrap0_resv0_0", 0x700001000, 0x1000 },
    { "am64x.debugss_wrap0_resv1_0", 0x700003000, 0x1000 },
    { "am64x.debugss_wrap0_rom_table_1_0", 0x720000000, 0x1000 },
    { "am64x.debugss_wrap0_cscti0", 0x720001000, 0x1000 },
    { "am64x.debugss_wrap0_drm_cfg_0", 0x720002000, 0x1000 },
    { "am64x.debugss_wrap0_resv3_0", 0x720003000, 0x1000 },
    { "am64x.debugss_wrap0_cstpiu_cfg_0", 0x720004000, 0x1000 },
    { "am64x.debugss_wrap0_ctf_cfg_0", 0x720005000, 0x1000 },
    { "am64x.debugss0_debug_cell_rom_slv", 0x73c020000, 0x1000 },
    { "am64x.debugss0_atb_replicator_cfg_cxatbreplicator_cfg", 0x73c024000, 0x1000 },
    { "am64x.debugss0_tbr_vbusp_wrap_tbr_cfg_tbr_cfg", 0x73c025000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c026000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c028000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c029000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c02a000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c02b000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c02c000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c02d000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c02e000, 0x1000 },
    { "am64x.debugss0_cscti", 0x73c02f000, 0x1000 },
    { "am64x.stm0_csstm", 0x73d200000, 0x1000 },
    { "am64x.stm0_cti_cscti", 0x73d201000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem0", 0x73e120000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem1", 0x73e121000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem2", 0x73e122000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem3", 0x73e123000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem4", 0x73e124000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem5", 0x73e125000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem6", 0x73e126000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem7", 0x73e127000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem8", 0x73e128000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem9", 0x73e129000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem10", 0x73e12a000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem11", 0x73e12b000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem12", 0x73e12c000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem13", 0x73e12d000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem14", 0x73e12e000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem15", 0x73e12f000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem16", 0x73e130000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem17", 0x73e131000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem18", 0x73e132000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem19", 0x73e133000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem20", 0x73e134000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem21", 0x73e135000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem22", 0x73e136000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem23", 0x73e137000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem24", 0x73e138000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem25", 0x73e139000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem26", 0x73e13a000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem27", 0x73e13b000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem28", 0x73e13c000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem29", 0x73e13d000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem30", 0x73e13e000, 0x1000 },
    { "am64x.cpt2_aggr0_cont_read_trc_src_vbus_mem31", 0x73e13f000, 0x1000 },
    { "am64x.debugss_wrap0_rom_table_0_1", 0x740000000, 0x1000 },
    { "am64x.debugss_wrap0_resv0_1", 0x740001000, 0x1000 },
    { "am64x.debugss_wrap0_resv1_1", 0x740003000, 0x1000 },
    { "am64x.debugss_wrap0_rom_table_1_1", 0x760000000, 0x1000 },
    { "am64x.debugss_wrap0_cscti1", 0x760001000, 0x1000 },
    { "am64x.debugss_wrap0_drm_cfg_1", 0x760002000, 0x1000 },
    { "am64x.debugss_wrap0_resv3_1", 0x760003000, 0x1000 },
    { "am64x.debugss_wrap0_cstpiu_cfg_1", 0x760004000, 0x1000 },
    { "am64x.debugss_wrap0_ctf_cfg_1", 0x760005000, 0x1000 },
    { "am64x.main_gpiomux_introuter0_intr_router_cfg", 0x000a00000, 0x800 },
    { "am64x.cmp_event_introuter0_intr_router_cfg", 0x000a30000, 0x800 },
    { "am64x.timesync_event_introuter0_intr_router_cfg", 0x000a40000, 0x800 },
    { "am64x.pru_icssg0_pa_stat_wrap_pa_slv_qstat", 0x030027000, 0x800 },
    { "am64x.pru_icssg0_pa_stat_wrap_pa_slv_cstat", 0x03002c000, 0x800 },
    { "am64x.pru_icssg1_pa_stat_wrap_pa_slv_qstat", 0x0300a7000, 0x800 },
    { "am64x.pru_icssg1_pa_stat_wrap_pa_slv_cstat", 0x0300ac000, 0x800 },
    { "am64x.dmsc0_pwr", 0x044130000, 0x800 },
    { "am64x.dmsc0_intaggr_imap", 0x044400000, 0x800 },
    { "am64x.dmass0_cbass_dmsc_isc", 0x045840000, 0x800 },
    { "am64x.dmass0_cbass_dmsc_qos", 0x045d40000, 0x800 },
    { "am64x.dmass0_bcdma_cred", 0x048400000, 0x800 },
    { "am64x.dbgsuspendrouter0_intr_router_cfg", 0x73d300000, 0x800 },
    { "am64x.psramecc0_ram", 0x000000000, 0x400 },
    { "am64x.cbass_dbg0_err", 0x000200000, 0x400 },
    { "am64x.cbass_infra1_err", 0x000210000, 0x400 },
    { "am64x.cbass_fw0_err", 0x000220000, 0x400 },
    { "am64x.pbist0_pbist", 0x000310000, 0x400 },
    { "am64x.compute_cluster0_pbist", 0x000330000, 0x400 },
    { "am64x.dftss0_mem", 0x000500000, 0x400 },
    { "am64x.ddpa0_k3_ddpa", 0x000580000, 0x400 },
    { "am64x.psramecc0_ecc_aggr", 0x000700000, 0x400 },
    { "am64x.ecc_aggr1_ecc_aggr", 0x000701000, 0x400 },
    { "am64x.usb0_ecc_aggr", 0x000703000, 0x400 },
    { "am64x.cpsw0_cpsw3_ecc", 0x000704000, 0x400 },
    { "am64x.mmcsd0_ecc_aggr", 0x000706000, 0x400 },
    { "am64x.mmcsd0_ecc_aggr", 0x000707000, 0x400 },
    { "am64x.mmcsd1_ecc_aggr", 0x000708000, 0x400 },
    { "am64x.mmcsd1_ecc_aggr", 0x000709000, 0x400 },
    { "am64x.sa2_ul0_ecc_aggr", 0x000712000, 0x400 },
    { "am64x.fss0_ecc_aggr", 0x000716000, 0x400 },
    { "am64x.compute_cluster0_ecc_aggr", 0x000717000, 0x400 },
    { "am64x.compute_cluster0_ecc_aggr", 0x000717400, 0x400 },
    { "am64x.compute_cluster0_ecc_aggr", 0x000717800, 0x400 },
    { "am64x.pcie0_core_ecc_aggr0", 0x000718000, 0x400 },
    { "am64x.pcie0_core_ecc_aggr1", 0x000719000, 0x400 },
    { "am64x.adc0_ecc_regs", 0x00071a000, 0x400 },
    { "am64x.gtc0_cfg0", 0x000a80000, 0x400 },
    { "am64x.vtm0_vtm_cfg1", 0x000b00000, 0x400 },
    { "am64x.vtm0_vtm_cfg2", 0x000b01000, 0x400 },
    { "am64x.vtm0_eccaggr_cfg", 0x000b02000, 0x400 },
    { "am64x.pdma0_regs", 0x000c00000, 0x400 },
    { "am64x.pdma1_regs", 0x000c01000, 0x400 },
    { "am64x.timer0_cfg", 0x002400000, 0x400 },
    { "am64x.timer1_cfg", 0x002410000, 0x400 },
    { "am64x.timer2_cfg", 0x002420000, 0x400 },
    { "am64x.timer3_cfg", 0x002430000, 0x400 },
    { "am64x.timer4_cfg", 0x002440000, 0x400 },
    { "am64x.timer5_cfg", 0x002450000, 0x400 },
    { "am64x.timer6_cfg", 0x002460000, 0x400 },
    { "am64x.timer7_cfg", 0x002470000, 0x400 },
    { "am64x.timer8_cfg", 0x002480000, 0x400 },
    { "am64x.timer9_cfg", 0x002490000, 0x400 },
    { "am64x.timer10_cfg", 0x0024a0000, 0x400 },
    { "am64x.timer11_cfg", 0x0024b0000, 0x400 },
    { "am64x.pbist3_pbist", 0x003310000, 0x400 },
    { "am64x.pbist2_pbist", 0x003330000, 0x400 },
    { "am64x.mcu_timeout0_cfg", 0x004300000, 0x400 },
    { "am64x.mcu_ecc_aggr0_ecc_aggr", 0x004700000, 0x400 },
    { "am64x.mcu_cbass0_err", 0x004720000, 0x400 },
    { "am64x.mcu_timer0_cfg", 0x004800000, 0x400 },
    { "am64x.mcu_timer1_cfg", 0x004810000, 0x400 },
    { "am64x.mcu_timer2_cfg", 0x004820000, 0x400 },
    { "am64x.mcu_timer3_cfg", 0x004830000, 0x400 },
    { "am64x.mcu_mcspi0_cfg", 0x004b00000, 0x400 },
    { "am64x.mcu_mcspi1_cfg", 0x004b10000, 0x400 },
    { "am64x.mcu_m4fss0_ecc_aggr", 0x005ff1000, 0x400 },
    { "am64x.pcie0_core_user_cfg_user_cfg", 0x00f100000, 0x400 },
    { "am64x.pcie0_core_cpts_cfg_cpts_vbusp", 0x00f103000, 0x400 },
    { "am64x.usb0_rams_inj_cfg", 0x00f901000, 0x400 },
    { "am64x.mmcsd1_mmc_sscfg", 0x00fa08000, 0x400 },
    { "am64x.mmcsd0_mmc_sscfg", 0x00fa18000, 0x400 },
    { "am64x.mcspi0_cfg", 0x020100000, 0x400 },
    { "am64x.mcspi1_cfg", 0x020110000, 0x400 },
    { "am64x.mcspi2_cfg", 0x020120000, 0x400 },
    { "am64x.mcspi3_cfg", 0x020130000, 0x400 },
    { "am64x.mcspi4_cfg", 0x020140000, 0x400 },
    { "am64x.mcan0_ecc_aggr", 0x024018000, 0x400 },
    { "am64x.mcan1_ecc_aggr", 0x024019000, 0x400 },
    { "am64x.adc0_fifo", 0x028000000, 0x400 },
    { "am64x.adc0", 0x028001000, 0x400 },
    { "am64x.cpts0_cpts", 0x039000000, 0x400 },
    { "am64x.cbass0_err", 0x03a000000, 0x400 },
    { "am64x.gpmc0_cfg", 0x03b000000, 0x400 },
    { "am64x.r5fss0_ecc_aggr", 0x03c010000, 0x400 },
    { "am64x.r5fss1_ecc_aggr", 0x03c030000, 0x400 },
    { "am64x.msram_256k0_ecc_aggr", 0x03f001000, 0x400 },
    { "am64x.msram_256k1_ecc_aggr", 0x03f002000, 0x400 },
    { "am64x.msram_256k2_ecc_aggr", 0x03f003000, 0x400 },
    { "am64x.gicss0_regs", 0x03f004000, 0x400 },
    { "am64x.dmass0_eccaggr", 0x03f005000, 0x400 },
    { "am64x.msram_256k5_ecc_aggr", 0x03f006000, 0x400 },
    { "am64x.msram_256k4_ecc_aggr", 0x03f007000, 0x400 },
    { "am64x.msram_256k3_ecc_aggr", 0x03f008000, 0x400 },
    { "am64x.dmsc0_ecc_aggr", 0x03f009000, 0x400 },
    { "am64x.pru_icssg0_ecc_aggr", 0x03f00a000, 0x400 },
    { "am64x.pru_icssg1_ecc_aggr", 0x03f00b000, 0x400 },
    { "am64x.r5fss0_ecc_aggr", 0x03f00d000, 0x400 },
    { "am64x.r5fss1_ecc_aggr", 0x03f00e000, 0x400 },
    { "am64x.ecc_aggr0_ecc_aggr", 0x03f00f000, 0x400 },
    { "am64x.msram_256k6_ecc_aggr", 0x03f010000, 0x400 },
    { "am64x.msram_256k7_ecc_aggr", 0x03f011000, 0x400 },
    { "am64x.pbist1_pbist", 0x03f100000, 0x400 },
    { "am64x.dmsc0_dmtimer0", 0x044133000, 0x400 },
    { "am64x.dmsc0_dmtimer1", 0x044134000, 0x400 },
    { "am64x.dmsc0_dmtimer2", 0x044238000, 0x400 },
    { "am64x.dmsc0_dmtimer3", 0x044239000, 0x400 },
    { "am64x.mcu_cbass0_isc", 0x045830000, 0x400 },
    { "am64x.dmass0_ringacc_isc", 0x045848000, 0x400 },
    { "am64x.dmsc0_cbass_glb", 0x045b00000, 0x400 },
    { "am64x.cbass_infra1_glb", 0x045b01000, 0x400 },
    { "am64x.mcu_cbass0_glb", 0x045b02000, 0x400 },
    { "am64x.dmass0_cbass_dmsc_glb", 0x045b03000, 0x400 },
    { "am64x.cbass0_glb", 0x045b08000, 0x400 },
    { "am64x.mcu_cbass0_qos", 0x045d30000, 0x400 },
    { "am64x.dmass0_intaggr_l2g", 0x048120000, 0x400 },
    { "am64x.dmass0_ringacc_gcfg", 0x048240000, 0x400 },
    { "am64x.pllctrl0_mem", 0x000410000, 0x200 },
    { "am64x.uart0", 0x002800000, 0x200 },
    { "am64x.uart1", 0x002810000, 0x200 },
    { "am64x.uart2", 0x002820000, 0x200 },
    { "am64x.uart3", 0x002830000, 0x200 },
    { "am64x.uart4", 0x002840000, 0x200 },
    { "am64x.uart5", 0x002850000, 0x200 },
    { "am64x.uart6", 0x002860000, 0x200 },
    { "am64x.mcu_pllctrl0_mem", 0x004020000, 0x200 },
    { "am64x.mcu_mcu_gpiomux_introuter0_intr_router_cfg", 0x004210000, 0x200 },
    { "am64x.mcu_uart0", 0x004a00000, 0x200 },
    { "am64x.mcu_uart1", 0x004a10000, 0x200 },
    { "am64x.ddr16ss0_emif_sscfg", 0x00f300000, 0x200 },
    { "am64x.fss0_ospi_cfg", 0x00fc44000, 0x200 },
    { "am64x.mcan0_cfg", 0x020701000, 0x200 },
    { "am64x.mcan1_cfg", 0x020711000, 0x200 },
    { "am64x.mailbox0_regs0", 0x029000000, 0x200 },
    { "am64x.mailbox0_regs1", 0x029010000, 0x200 },
    { "am64x.mailbox0_regs2", 0x029020000, 0x200 },
    { "am64x.mailbox0_regs3", 0x029030000, 0x200 },
    { "am64x.mailbox0_regs4", 0x029040000, 0x200 },
    { "am64x.mailbox0_regs5", 0x029050000, 0x200 },
    { "am64x.mailbox0_regs6", 0x029060000, 0x200 },
    { "am64x.mailbox0_regs7", 0x029070000, 0x200 },
    { "am64x.pru_icssg0_pr1_cfg_slv", 0x030026000, 0x200 },
    { "am64x.pru_icssg1_pr1_cfg_slv", 0x0300a6000, 0x200 },
    { "am64x.timermgr0_config", 0x03cd00000, 0x200 },
    { "am64x.sa2_ul0_sa_ul_updates", 0x040901000, 0x200 },
    { "am64x.dmass0_psilcfg_proxy", 0x048130000, 0x200 },
    { "am64x.efuse0_mem", 0x000300000, 0x100 },
    { "am64x.gpio0", 0x000600000, 0x100 },
    { "am64x.gpio1", 0x000601000, 0x100 },
    { "am64x.mcu_gpio0", 0x004201000, 0x100 },
    { "am64x.mcu_rti0_cfg", 0x004880000, 0x100 },
    { "am64x.mcu_i2c0_cfg", 0x004900000, 0x100 },
    { "am64x.mcu_i2c1_cfg", 0x004910000, 0x100 },
    { "am64x.rti0_cfg", 0x00e000000, 0x100 },
    { "am64x.rti1_cfg", 0x00e010000, 0x100 },
    { "am64x.rti8_cfg", 0x00e080000, 0x100 },
    { "am64x.rti9_cfg", 0x00e090000, 0x100 },
    { "am64x.rti10_cfg", 0x00e0a0000, 0x100 },
    { "am64x.rti11_cfg", 0x00e0b0000, 0x100 },
    { "am64x.usb0_mmr_mmrvbp_usbss_cmn", 0x00f900000, 0x100 },
    { "am64x.fss0_fss_genregs", 0x00fc00000, 0x100 },
    { "am64x.fss0_fsas_genregs", 0x00fc10000, 0x100 },
    { "am64x.fss0_ospi_flash_cfg", 0x00fc40000, 0x100 },
    { "am64x.i2c0_cfg", 0x020000000, 0x100 },
    { "am64x.i2c1_cfg", 0x020010000, 0x100 },
    { "am64x.i2c2_cfg", 0x020020000, 0x100 },
    { "am64x.i2c3_cfg", 0x020030000, 0x100 },
    { "am64x.mcan0_ss", 0x020700000, 0x100 },
    { "am64x.mcan1_ss", 0x020710000, 0x100 },
    { "am64x.epwm0_epwm", 0x023000000, 0x100 },
    { "am64x.epwm1_epwm", 0x023010000, 0x100 },
    { "am64x.epwm2_epwm", 0x023020000, 0x100 },
    { "am64x.epwm3_epwm", 0x023030000, 0x100 },
    { "am64x.epwm4_epwm", 0x023040000, 0x100 },
    { "am64x.epwm5_epwm", 0x023050000, 0x100 },
    { "am64x.epwm6_epwm", 0x023060000, 0x100 },
    { "am64x.epwm7_epwm", 0x023070000, 0x100 },
    { "am64x.epwm8_epwm", 0x023080000, 0x100 },
    { "am64x.ecap0_ctl_sts", 0x023100000, 0x100 },
    { "am64x.ecap1_ctl_sts", 0x023110000, 0x100 },
    { "am64x.ecap2_ctl_sts", 0x023120000, 0x100 },
    { "am64x.eqep0_reg", 0x023200000, 0x100 },
    { "am64x.eqep1_reg", 0x023210000, 0x100 },
    { "am64x.eqep2_reg", 0x023220000, 0x100 },
    { "am64x.fsirx0_fsi_rx_cfg", 0x023500000, 0x100 },
    { "am64x.fsirx1_fsi_rx_cfg", 0x023510000, 0x100 },
    { "am64x.fsirx2_fsi_rx_cfg", 0x023520000, 0x100 },
    { "am64x.fsirx3_fsi_rx_cfg", 0x023530000, 0x100 },
    { "am64x.fsirx4_fsi_rx_cfg", 0x023540000, 0x100 },
    { "am64x.fsirx5_fsi_rx_cfg", 0x023550000, 0x100 },
    { "am64x.fsitx0_fsi_tx_cfg", 0x023600000, 0x100 },
    { "am64x.fsitx1_fsi_tx_cfg", 0x023610000, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp0_iram", 0x030022000, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp0_iram_debug", 0x030022400, 0x100 },
    { "am64x.pru_icssg0_pr1_rtu0_pr1_rtu0_iram", 0x030023000, 0x100 },
    { "am64x.pru_icssg0_pr1_rtu0_pr1_rtu0_iram_debug", 0x030023400, 0x100 },
    { "am64x.pru_icssg0_pr1_rtu1_pr1_rtu1_iram", 0x030023800, 0x100 },
    { "am64x.pru_icssg0_pr1_rtu1_pr1_rtu1_iram_debug", 0x030023c00, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp1_iram", 0x030024000, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp1_iram_debug", 0x030024400, 0x100 },
    { "am64x.pru_icssg0_pr1_protect_slv", 0x030024c00, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp_tx0_iram", 0x030025000, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp_tx0_iram_debug", 0x030025400, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp_tx1_iram", 0x030025800, 0x100 },
    { "am64x.pru_icssg0_pr1_pdsp_tx1_iram_debug", 0x030025c00, 0x100 },
    { "am64x.pru_icssg0_pr1_tasks_mgr_pru0_pr1_tasks_mgr_pru0_mmr", 0x03002a000, 0x100 },
    { "am64x.pru_icssg0_pr1_tasks_mgr_rtu0_pr1_tasks_mgr_rtu0_mmr", 0x03002a100, 0x100 },
    { "am64x.pru_icssg0_pr1_tasks_mgr_pru1_pr1_tasks_mgr_pru1_mmr", 0x03002a200, 0x100 },
    { "am64x.pru_icssg0_pr1_tasks_mgr_rtu1_pr1_tasks_mgr_rtu1_mmr", 0x03002a300, 0x100 },
    { "am64x.pru_icssg0_pr1_tasks_mgr_pru_tx0_pr1_tasks_mgr_pru_tx0_mmr", 0x03002a400, 0x100 },
    { "am64x.pru_icssg0_pr1_tasks_mgr_pru_tx1_pr1_tasks_mgr_pru_tx1_mmr", 0x03002a500, 0x100 },
    { "am64x.pru_icssg0_pr1_icss_ecap0_ecap_slv", 0x030030000, 0x100 },
    { "am64x.pru_icssg0_pr1_mii_rt_pr1_mii_rt_cfg", 0x030032000, 0x100 },
    { "am64x.pru_icssg0_pr1_mii_rt_pr1_sgmii0_cfg_sgmii0", 0x030032100, 0x100 },
    { "am64x.pru_icssg0_pr1_mii_rt_pr1_sgmii1_cfg_sgmii1", 0x030032200, 0x100 },
    { "am64x.pru_icssg0_pr1_mdio_v1p7_mdio", 0x030032400, 0x100 },
    { "am64x.pru_icssg0_pa_stat_wrap_pa_slv", 0x03003c000, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp0_iram", 0x0300a2000, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp0_iram_debug", 0x0300a2400, 0x100 },
    { "am64x.pru_icssg1_pr1_rtu0_pr1_rtu0_iram", 0x0300a3000, 0x100 },
    { "am64x.pru_icssg1_pr1_rtu0_pr1_rtu0_iram_debug", 0x0300a3400, 0x100 },
    { "am64x.pru_icssg1_pr1_rtu1_pr1_rtu1_iram", 0x0300a3800, 0x100 },
    { "am64x.pru_icssg1_pr1_rtu1_pr1_rtu1_iram_debug", 0x0300a3c00, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp1_iram", 0x0300a4000, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp1_iram_debug", 0x0300a4400, 0x100 },
    { "am64x.pru_icssg1_pr1_protect_slv", 0x0300a4c00, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp_tx0_iram", 0x0300a5000, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp_tx0_iram_debug", 0x0300a5400, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp_tx1_iram", 0x0300a5800, 0x100 },
    { "am64x.pru_icssg1_pr1_pdsp_tx1_iram_debug", 0x0300a5c00, 0x100 },
    { "am64x.pru_icssg1_pr1_tasks_mgr_pru0_pr1_tasks_mgr_pru0_mmr", 0x0300aa000, 0x100 },
    { "am64x.pru_icssg1_pr1_tasks_mgr_rtu0_pr1_tasks_mgr_rtu0_mmr", 0x0300aa100, 0x100 },
    { "am64x.pru_icssg1_pr1_tasks_mgr_pru1_pr1_tasks_mgr_pru1_mmr", 0x0300aa200, 0x100 },
    { "am64x.pru_icssg1_pr1_tasks_mgr_rtu1_pr1_tasks_mgr_rtu1_mmr", 0x0300aa300, 0x100 },
    { "am64x.pru_icssg1_pr1_tasks_mgr_pru_tx0_pr1_tasks_mgr_pru_tx0_mmr", 0x0300aa400, 0x100 },
    { "am64x.pru_icssg1_pr1_tasks_mgr_pru_tx1_pr1_tasks_mgr_pru_tx1_mmr", 0x0300aa500, 0x100 },
    { "am64x.pru_icssg1_pr1_icss_ecap0_ecap_slv", 0x0300b0000, 0x100 },
    { "am64x.pru_icssg1_pr1_mii_rt_pr1_mii_rt_cfg", 0x0300b2000, 0x100 },
    { "am64x.pru_icssg1_pr1_mii_rt_pr1_sgmii0_cfg_sgmii0", 0x0300b2100, 0x100 },
    { "am64x.pru_icssg1_pr1_mii_rt_pr1_sgmii1_cfg_sgmii1", 0x0300b2200, 0x100 },
    { "am64x.pru_icssg1_pr1_mdio_v1p7_mdio", 0x0300b2400, 0x100 },
    { "am64x.pru_icssg1_pa_stat_wrap_pa_slv", 0x0300bc000, 0x100 },
    { "am64x.r5fss0_evnt_bus_vbusp_mmrs", 0x03c018000, 0x100 },
    { "am64x.r5fss1_evnt_bus_vbusp_mmrs", 0x03c038000, 0x100 },
    { "am64x.dmsc0_rti", 0x044135000, 0x100 },
    { "am64x.dmsc0_rti", 0x044135100, 0x100 },
    { "am64x.dmass0_sec_proxy_mmrs", 0x048250000, 0x100 },
    { "am64x.dmass0_pktdma_gcfg", 0x0485c0000, 0x100 },
    { "am64x.dmass0_bcdma_gcfg", 0x0485c0100, 0x100 },
    { "am64x.debugss_wrap0_cfgap_cfg_0", 0x700002000, 0x100 },
    { "am64x.debugss_wrap0_apbap_cfg_0", 0x700002100, 0x100 },
    { "am64x.debugss_wrap0_axiap_cfg_0", 0x700002200, 0x100 },
    { "am64x.debugss_wrap0_pwrap_cfg_0", 0x700002300, 0x100 },
    { "am64x.debugss_wrap0_pview_cfg_0", 0x700002400, 0x100 },
    { "am64x.debugss_wrap0_jtagap_cfg_0", 0x700002500, 0x100 },
    { "am64x.debugss_wrap0_secap_cfg_0", 0x700002600, 0x100 },
    { "am64x.debugss_wrap0_cortex0_cfg_0", 0x700002700, 0x100 },
    { "am64x.debugss_wrap0_cortex1_cfg_0", 0x700002800, 0x100 },
    { "am64x.debugss_wrap0_cortex2_cfg_0", 0x700002900, 0x100 },
    { "am64x.debugss_wrap0_cortex3_cfg_0", 0x700002a00, 0x100 },
    { "am64x.debugss_wrap0_cortex4_cfg_0", 0x700002b00, 0x100 },
    { "am64x.debugss_wrap0_cortex5_cfg_0", 0x700002c00, 0x100 },
    { "am64x.debugss_wrap0_cortex6_cfg_0", 0x700002d00, 0x100 },
    { "am64x.debugss_wrap0_cortex7_cfg_0", 0x700002e00, 0x100 },
    { "am64x.debugss_wrap0_cortex8_cfg_0", 0x700002f00, 0x100 },
    { "am64x.cpt2_aggr0_mmr_slv", 0x73e100000, 0x100 },
    { "am64x.cpt2_aggr0_vbusp2apb_wrap_stp2atb_vbus_cfg", 0x73e100100, 0x100 },
    { "am64x.debugss_wrap0_cfgap_cfg_1", 0x740002000, 0x100 },
    { "am64x.debugss_wrap0_apbap_cfg_1", 0x740002100, 0x100 },
    { "am64x.debugss_wrap0_axiap_cfg_1", 0x740002200, 0x100 },
    { "am64x.debugss_wrap0_pwrap_cfg_1", 0x740002300, 0x100 },
    { "am64x.debugss_wrap0_pview_cfg_1", 0x740002400, 0x100 },
    { "am64x.debugss_wrap0_jtagap_cfg_1", 0x740002500, 0x100 },
    { "am64x.debugss_wrap0_secap_cfg_1", 0x740002600, 0x100 },
    { "am64x.debugss_wrap0_cortex0_cfg_1", 0x740002700, 0x100 },
    { "am64x.debugss_wrap0_cortex1_cfg_1", 0x740002800, 0x100 },
    { "am64x.debugss_wrap0_cortex2_cfg_1", 0x740002900, 0x100 },
    { "am64x.debugss_wrap0_cortex3_cfg_1", 0x740002a00, 0x100 },
    { "am64x.debugss_wrap0_cortex4_cfg_1", 0x740002b00, 0x100 },
    { "am64x.debugss_wrap0_cortex5_cfg_1", 0x740002c00, 0x100 },
    { "am64x.debugss_wrap0_cortex6_cfg_1", 0x740002d00, 0x100 },
    { "am64x.debugss_wrap0_cortex7_cfg_1", 0x740002e00, 0x100 },
    { "am64x.debugss_wrap0_cortex8_cfg_1", 0x740002f00, 0x100 },
    { "am64x.sa2_ul0_sa_ul_rng", 0x040910000, 0x80 },
    { "am64x.dcc0_dcc2", 0x000800000, 0x40 },
    { "am64x.dcc1_dcc2", 0x000804000, 0x40 },
    { "am64x.dcc2_dcc2", 0x000808000, 0x40 },
    { "am64x.dcc3_dcc2", 0x00080c000, 0x40 },
    { "am64x.dcc4_dcc2", 0x000810000, 0x40 },
    { "am64x.dcc5_dcc2", 0x000814000, 0x40 },
    { "am64x.mcu_dcc0_dcc2", 0x004c00000, 0x40 },
    { "am64x.pru_icssg0_pr1_icss_uart_uart_slv", 0x030028000, 0x40 },
    { "am64x.pru_icssg1_pr1_icss_uart_uart_slv", 0x0300a8000, 0x40 },
    { "am64x.dmsc0_intaggr_cfg", 0x044410000, 0x20 },
    { "am64x.dmass0_intaggr_cfg", 0x048110000, 0x20 },
};

typedef struct TiAm64xState {
    MachineState parent_obj;

    DeviceState *gic;
    MemoryRegion msram;
    struct arm_boot_info binfo;
} TiAm64xState;

#define TYPE_TI_AM64X_MACHINE MACHINE_TYPE_NAME("ti-am64x")
OBJECT_DECLARE_SIMPLE_TYPE(TiAm64xState, TI_AM64X_MACHINE)

/* Minimal DT; -dtb replaces it. boot.c adds memory, psci and chosen. */
static void *am64x_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    void *fdt = create_device_tree(fdt_size);
    const char uart_compat[] = "ns16550a";
    uint32_t gic_ph = 1, clk_ph = 2;
    char *n;

    if (!fdt) {
        return NULL;
    }
    qemu_fdt_setprop_string(fdt, "/", "compatible", "ti,am642");
    qemu_fdt_setprop_string(fdt, "/", "model", "QEMU ti-am64x");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", gic_ph);

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "/uart@2800000");
    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0", "/uart@2800000");

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    for (int i = 0; i < AM64X_NUM_CPUS; i++) {
        n = g_strdup_printf("/cpus/cpu@%d", i);
        qemu_fdt_add_subnode(fdt, n);
        qemu_fdt_setprop_string(fdt, n, "device_type", "cpu");
        qemu_fdt_setprop_string(fdt, n, "compatible", "arm,cortex-a53");
        qemu_fdt_setprop_string(fdt, n, "enable-method", "psci");
        qemu_fdt_setprop_cell(fdt, n, "reg", i);
        g_free(n);
    }

    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop(fdt, "/timer", "always-on", NULL, 0);
    /* PPI 13,14,11,10 = secure phys, phys, virt, hyp */
    qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                           1, 13, 0xf04, 1, 14, 0xf04,
                           1, 11, 0xf04, 1, 10, 0xf04);

    qemu_fdt_add_subnode(fdt, "/gic");
    qemu_fdt_setprop_string(fdt, "/gic", "compatible", "arm,gic-v3");
    qemu_fdt_setprop_cell(fdt, "/gic", "#interrupt-cells", 3);
    qemu_fdt_setprop(fdt, "/gic", "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, "/gic", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/gic", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/gic", "ranges", NULL, 0);
    qemu_fdt_setprop_cell(fdt, "/gic", "phandle", gic_ph);
    qemu_fdt_setprop_sized_cells(fdt, "/gic", "reg",
                                 2, AM64X_GICD_BASE, 2, 0x10000,
                                 2, AM64X_GICR_BASE,
                                 2, AM64X_GICR_STRIDE * AM64X_NUM_CPUS);

    qemu_fdt_add_subnode(fdt, "/uartclk");
    qemu_fdt_setprop_string(fdt, "/uartclk", "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, "/uartclk", "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/uartclk", "clock-frequency", AM64X_UART_CLK);
    qemu_fdt_setprop_cell(fdt, "/uartclk", "phandle", clk_ph);

    for (int i = 0; i < AM64X_NUM_UARTS; i++) {
        hwaddr base = AM64X_UART_BASE + i * AM64X_UART_STRIDE;

        n = g_strdup_printf("/uart@%" HWADDR_PRIx, base);
        qemu_fdt_add_subnode(fdt, n);
        qemu_fdt_setprop(fdt, n, "compatible", uart_compat, sizeof(uart_compat));
        qemu_fdt_setprop_sized_cells(fdt, n, "reg", 2, base, 2, 0x200);
        qemu_fdt_setprop_cell(fdt, n, "reg-shift", 2);
        qemu_fdt_setprop_cell(fdt, n, "reg-io-width", 4);
        qemu_fdt_setprop_cell(fdt, n, "clocks", clk_ph);
        qemu_fdt_setprop_cells(fdt, n, "interrupts", 0, AM64X_UART_IRQ0 + i, 4);
        if (i) {
            qemu_fdt_setprop_string(fdt, n, "status", "disabled");
        }
        g_free(n);
    }
    return fdt;
}

static void ti_am64x_init(MachineState *machine)
{
    TiAm64xState *s = TI_AM64X_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    SysBusDevice *gicbus;
    QList *redist;

    if (machine->ram_size > AM64X_DDR_MAX) {
        error_report("ti-am64x: DDR window is at most 2 GiB");
        exit(1);
    }

    for (int i = 0; i < AM64X_NUM_CPUS; i++) {
        Object *cpu = object_new(machine->cpu_type);

        object_property_set_int(cpu, "mp-affinity", i, &error_abort);
        object_property_set_bool(cpu, "has_el3", false, NULL);
        object_property_set_bool(cpu, "has_el2", false, NULL);
        object_property_set_int(cpu, "psci-conduit", QEMU_PSCI_CONDUIT_SMC,
                                &error_abort);
        if (i) {
            object_property_set_bool(cpu, "start-powered-off", true,
                                     &error_abort);
        }
        if (tcg_enabled()) {
            object_property_set_int(cpu, "cntfrq", AM64X_GTC_HZ, &error_abort);
        }
        qdev_realize_and_unref(DEVICE(cpu), NULL, &error_fatal);
    }

    memory_region_add_subregion(sysmem, AM64X_DDR_BASE, machine->ram);
    memory_region_init_ram(&s->msram, NULL, "am64x.msram",
                           AM64X_MSRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, AM64X_MSRAM_BASE, &s->msram);

    for (int i = 0; i < G_N_ELEMENTS(am64x_blocks); i++) {
        create_unimplemented_device(am64x_blocks[i].name, am64x_blocks[i].base,
                                    am64x_blocks[i].size);
    }

    s->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", AM64X_NUM_CPUS);
    qdev_prop_set_uint32(s->gic, "num-irq", AM64X_NUM_IRQS + 32);
    redist = qlist_new();
    qlist_append_int(redist, AM64X_NUM_CPUS);
    qdev_prop_set_array(s->gic, "redist-region-count", redist);
    gicbus = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbus, &error_fatal);
    sysbus_mmio_map(gicbus, 0, AM64X_GICD_BASE);
    sysbus_mmio_map(gicbus, 1, AM64X_GICR_BASE);

    for (int i = 0; i < AM64X_NUM_CPUS; i++) {
        DeviceState *cpu = DEVICE(qemu_get_cpu(i));
        int base = AM64X_NUM_IRQS + i * GIC_INTERNAL;
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        };

        for (int t = 0; t < ARRAY_SIZE(timer_irq); t++) {
            qdev_connect_gpio_out(cpu, t, qdev_get_gpio_in(s->gic,
                                  base + timer_irq[t]));
        }
        sysbus_connect_irq(gicbus, i, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbus, i + AM64X_NUM_CPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbus, i + 2 * AM64X_NUM_CPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbus, i + 3 * AM64X_NUM_CPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
    }

    for (int i = 0; i < AM64X_NUM_UARTS; i++) {
        serial_mm_init(sysmem, AM64X_UART_BASE + i * AM64X_UART_STRIDE, 2,
                       qdev_get_gpio_in(s->gic, AM64X_UART_IRQ0 + i),
                       AM64X_UART_CLK / 16, serial_hd(i), DEVICE_LITTLE_ENDIAN);
    }

    s->binfo.ram_size = machine->ram_size;
    s->binfo.loader_start = AM64X_DDR_BASE;
    s->binfo.get_dtb = am64x_dtb;
    s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &s->binfo);
}

/* Userspace GICv3 under HVF by default, like virt; kernel-irqchip=on opts in. */
static bool am64x_kernel_irqchip_default(const MachineState *ms)
{
    return !hvf_enabled();
}

static const char *am64x_default_cpu_type(const MachineState *ms)
{
    return tcg_enabled() ? ARM_CPU_TYPE_NAME("cortex-a53")
                         : ARM_CPU_TYPE_NAME("max");
}

static void ti_am64x_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "TI AM64x (AM6442) Cortex-A53 cluster";
    mc->init = ti_am64x_init;
    mc->max_cpus = AM64X_NUM_CPUS;
    mc->default_cpus = AM64X_NUM_CPUS;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "am64x.ddr";
    mc->get_default_cpu_type = am64x_default_cpu_type;
    mc->get_kernel_irqchip_default = am64x_kernel_irqchip_default;
    mc->block_default_type = IF_NONE;
}

static const TypeInfo ti_am64x_info = {
    .name = TYPE_TI_AM64X_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(TiAm64xState),
    .class_init = ti_am64x_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void ti_am64x_register_types(void)
{
    type_register_static(&ti_am64x_info);
}

type_init(ti_am64x_register_types)
