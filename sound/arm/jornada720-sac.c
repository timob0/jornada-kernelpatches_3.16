/*
 *  jornada720-sac.h
 *
 *  Register interface to SA1111 Serial Audio Controller and L3 bus
 *
 *  Copyright (C) 2021 Timo Biesenbach
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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
#include <linux/kern_levels.h>
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

#include "jornada720-sac.h"

#undef DEBUG
// #define DEBUG

static DEFINE_SPINLOCK(snd_jornada720_sa1111_sac_lock);

// SA1111 Sound Controller register write interface
void         sa1111_sac_writereg(struct sa1111_dev *devptr, unsigned int val, u32 reg) {
	sa1111_writel(val, devptr->mapbase + reg);
}

// SA1111 Sound Controller register read interface
unsigned int sa1111_sac_readreg(struct sa1111_dev *devptr, u32 reg) {
	return sa1111_readl(devptr->mapbase + reg);
}

// Send bytes via SA1111-L3
void 		   sa1111_l3_send_byte(struct sa1111_dev *devptr, unsigned char addr, unsigned char dat) {
	int i=0;
	unsigned int SASCR;
	unsigned int SACR1;
	
	// Make sure only one thread is in the critical section below.
	spin_lock(&snd_jornada720_sa1111_sac_lock);
	
	sa1111_sac_writereg(devptr, 0, SA1111_L3_CAR);  // <- why is this needed? check datasheet
	sa1111_sac_writereg(devptr, 0, SA1111_L3_CDR);  // <- why is this needed? check datasheet
	mdelay(1);

	SASCR = sa1111_sac_readreg(devptr, SA1111_SASCR);
	SASCR = SASCR_DTS|SASCR_RDD;
	sa1111_sac_writereg(devptr, SASCR, SA1111_SASCR);

	sa1111_sac_writereg(devptr, addr,  SA1111_L3_CAR);
	sa1111_sac_writereg(devptr, dat,   SA1111_L3_CDR);

	while (((sa1111_sac_readreg(devptr, SA1111_SASR0) & SASR0_L3WD) == 0) && (i < 1000)) {
		mdelay(1); // one msec seems to long
		i++;
	}
	if (((sa1111_sac_readreg(devptr, SA1111_SASR0) & SASR0_L3WD) == 0)) {
		printk(KERN_ERR "Jornada 720 soundcard L3 timeout! Programming error: Make sure SA1111 L3 Clock and bus are enabled bedore using L3 bus.\n");
	}
	
	SASCR = SASCR_DTS|SASCR_RDD;
	sa1111_sac_writereg(devptr, SASCR, SA1111_SASCR);
	
	// Give up the lock
	spin_unlock(&snd_jornada720_sa1111_sac_lock);
}
