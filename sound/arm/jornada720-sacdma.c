/*
 *  linux/arch/arm/mach-sa1100/dma-sa1111.c
 *
 *  Copyright (C) 2000 John Dorsey
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  4 September 2000 - created.
 */
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/types.h>
// Hardware stuff
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <mach/jornada720.h>
#include <mach/hardware.h>
#include <asm/mach-types.h>
#include <asm/hardware/sa1111.h>

#include "jornada720-sacdma.h"

#undef DEBUG
// #define DEBUG

/* Our DMA channels */
sa1100_dma_t dma_chan[SA1111_SAC_DMA_CHANNELS];

// SA1111 Sound Controller interface
static inline void         sa1111_sac_writereg(struct sa1111_dev *devptr, unsigned int val, u32 reg) {
	sa1111_writel(val, devptr->mapbase + reg);
}

static inline unsigned int sa1111_sac_readreg(struct sa1111_dev *devptr, u32 reg) {
	return sa1111_readl(devptr->mapbase + reg);
}


/*
 * Control register structure for the SA1111 SAC DMA.
 * As per the datasheet there is no such thing as "reset". Also several
 * register bits are read only.
 * We'll clear some bits and set addresses and counts to zero.
 */
void sa1111_reset_sac_dma(struct sa1111_dev *devptr) {
	unsigned int val;
	val = 0;
	// TX
	sa1111_sac_writereg(devptr, val, SA1111_SADTCS); // Zero control register
	sa1111_sac_writereg(devptr, val, SA1111_SADTSA); // Zero address A register
	sa1111_sac_writereg(devptr, val, SA1111_SADTCA); // Zero count A register
	sa1111_sac_writereg(devptr, val, SA1111_SADTSB); // Zero address B register
	sa1111_sac_writereg(devptr, val, SA1111_SADTCB); // Zero count B register

	//RX
	sa1111_sac_writereg(devptr, val, SA1111_SADRCS);
	sa1111_sac_writereg(devptr, val, SA1111_SADRSA); // Zero address A register
	sa1111_sac_writereg(devptr, val, SA1111_SADRCA); // Zero count A register
	sa1111_sac_writereg(devptr, val, SA1111_SADRSB); // Zero address B register
	sa1111_sac_writereg(devptr, val, SA1111_SADRCB); // Zero count B register
	printk(KERN_INFO "j720 sa1111 SAC DMA registers reset.\n");

	// Program SACR0 DMA Thresholds
	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val = val & 0xFF; //Mask out bits 8-31
	val = val | (0x07 << 8);    // set TFTH to 7 (transmit fifo threshold)
	val = val | (0x07 << 12);   // set RFTH to 7 (receive  fifo threshold)
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);
	printk(KERN_INFO "j720 sa1111 SAC SACRO 0x%lxh\n", val);
	printk(KERN_INFO "j720 sa1111 SAC reset and enabled\n");
}
/* Start a DMA transfer to/from dma_ptr with size bytes 
 * either in transmit or receive direction
 *   SA1111_SADTCS		0x34 <<- tx control register
	 SA1111_SADTSA		0x38 <<- DMA tx buffer a start address
	 SA1111_SADTCA		0x3c <<- DMA tx buffer a count
	 SA1111_SADTSB		0x40 <<- DMA tx buffer b start address
	 SA1111_SADTCB		0x44 <<- DMA tx buffer b count

	 SA1111_SADRCS		0x48 <<- tx control register
	 SA1111_SADRSA		0x4c <<- DMA Rx buffer a start address
	 SA1111_SADRCA		0x50 <<- DMA Rx buffer a count
	 SA1111_SADRSB		0x54 <<- DMA Rx buffer b start address
	 SA1111_SADRCB		0x58 <<- DMA Rx buffer b count

	Logic:
	Register base address to use = 0x34 + direction (0x14) 
	         channel addr/count:   + channel (0x08)
 */

// Each dma start will increment this counter (one per direction). If even use ch A, if odd use B
static unsigned int dma_cnt[2] = {0, 0};

/* Find out if dma for the given direction is finished
 * will figure out the channel (A or B) based on the dma_cnt
 */
int done_sa1111_sac_dma(struct sa1111_dev *devptr, int direction) {
	unsigned int val;
	unsigned int REG_CS    = SA1111_SADTCS + (direction * DMA_REG_RX_OFS);  // Control register

	// read status register
	val = sa1111_sac_readreg(devptr, REG_CS);
	if (dma_cnt[direction] % 2) {
		// Channel B
		if (val & SAD_CS_DBDB) return 1;
	} 
	else  {
		// Channel A
		if (val & SAD_CS_DBDA) return 1;
	}

	return 0;
}

int start_sa1111_sac_dma(struct sa1111_dev *devptr, dma_addr_t dma_ptr, size_t size, int direction) {
	unsigned int val;
	unsigned int REG_CS    = SA1111_SADTCS + (direction * DMA_REG_RX_OFS);  // Control register
	unsigned int REG_ADDR  = SA1111_SADTSA + (direction * DMA_REG_RX_OFS);  // Address register
	unsigned int REG_COUNT = SA1111_SADTCA + (direction * DMA_REG_RX_OFS);  // Count register

	/* Make sure direction is 0|1 */
	if (direction<0 || direction>1) {
		printk("Invalid direction %d\n", direction);
		return -1;
	}

	/* Read control register */
	val = sa1111_sac_readreg(devptr, REG_CS);

	// we will alternate between channels A and B
	// this might or might not need be done for each direction separately.
	if (++dma_cnt[direction] % 2) {
		// Add offset for channel b registers to address / count regs
		REG_ADDR  += DMA_CH_B;
		REG_COUNT += DMA_CH_B;

		// update control reg value
		val |= SAD_CS_DSTB | SAD_CS_DEN;
		#ifdef DEBUG
		printk(" using DMA channel B\n");

		printk(" using DMA address reg 0x%lxh\n", REG_ADDR);
		printk(" using DMA count   reg 0x%lxh\n", REG_COUNT);
		printk(" using DMA control reg 0x%lxh\n", REG_CS);

		printk(" using DMA address     0x%lxh\n", dma_ptr);
		printk(" using DMA count       0x%lxh\n", size);
		printk(" using DMA control     0x%lxh\n", val);
		#endif
		sa1111_sac_writereg(devptr, dma_ptr, REG_ADDR);
		sa1111_sac_writereg(devptr, size, REG_COUNT);
		sa1111_sac_writereg(devptr, val, REG_CS);
	} 
	else {
		REG_ADDR  += DMA_CH_A;
		REG_COUNT += DMA_CH_A;

		// update control reg value
		val |= SAD_CS_DSTA | SAD_CS_DEN;
		#ifdef DEBUG
		printk(" using DMA channel A\n");

		printk(" using DMA address reg 0x%lxh\n", REG_ADDR);
		printk(" using DMA count   reg 0x%lxh\n", REG_COUNT);
		printk(" using DMA control reg 0x%lxh\n", REG_CS);

		printk(" using DMA address     0x%lxh\n", dma_ptr);
		printk(" using DMA count       0x%lxh\n", size);
		printk(" using DMA control     0x%lxh\n", val);
		#endif
		sa1111_sac_writereg(devptr, dma_ptr, REG_ADDR);
		sa1111_sac_writereg(devptr, size, REG_COUNT);
		sa1111_sac_writereg(devptr, val, REG_CS);
	}
	return 0;
}
