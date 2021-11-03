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

#include "jornada720-common.h"
#include "jornada720-sac.h"

// SAC module lock
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

	sa1111_sac_writereg(devptr, addr,  SA1111_L3_CAR);
	sa1111_sac_writereg(devptr, dat,   SA1111_L3_CDR);

	while (((sa1111_sac_readreg(devptr, SA1111_SASR0) & SASR0_L3WD) == 0) && (i < 1000)) {
		mdelay(1); // one msec seems to long
		i++;
	}
	if (((sa1111_sac_readreg(devptr, SA1111_SASR0) & SASR0_L3WD) == 0)) {
		printk(KERN_ERR "Jornada 720 soundcard L3 timeout! Programming error: Make sure SA1111 L3 Clock and bus are enabled before using L3 bus.\n");
	}
	
	SASCR = SASCR_DTS;
	sa1111_sac_writereg(devptr, SASCR, SA1111_SASCR);
	
	// Give up the lock
	spin_unlock(&snd_jornada720_sa1111_sac_lock);
}

// Will initialize the SA1111 and its L3 hardware
void sa1111_audio_init(struct sa1111_dev *devptr) {
	// For register bitbanging
	unsigned int val; 

	// Get access to the "parent" sa1111 chip 
	struct sa1111 *sachip = get_sa1111_base_drv(devptr);

	DPRINTK(KERN_INFO "j720 sa1111 init...");
	DPRINTK(KERN_INFO "j720 sa1111 device id: %d\n", devptr->devid);
	DPRINTK(KERN_INFO "j720 sa1111 chip base: 0x%lxh\n", sachip->base);
	DPRINTK(KERN_INFO "j720 sa1111 SAC  base: 0x%lxh\n", devptr->mapbase);

	// Make sure only one thread is in the critical section below.
	spin_lock(&snd_jornada720_sa1111_sac_lock);
	
	PPSR &= ~(PPC_LDD3 | PPC_LDD4);
	PPDR |= PPC_LDD3 | PPC_LDD4;
	PPSR |= PPC_LDD4; /* enable speaker */
	PPSR |= PPC_LDD3; /* enable microphone */
	DPRINTK(KERN_INFO "j720 sa1111 speaker/mic pre-amps enabled\n");
	
	// deselect AC Link
	sa1111_select_audio_mode(devptr, SA1111_AUDIO_I2S);
	DPRINTK(KERN_INFO "j720 sa1111 I2S protocol enabled\n");

	/* Enable the I2S clock and L3 bus clock. This is a function in another SA1111 block
	 * which is why we need the sachip stuff (should probably be a function in sa1111.c/h)
	 */
	val = sa1111_readl(sachip->base + SA1111_SKPCR);
	val|= (SKPCR_I2SCLKEN | SKPCR_L3CLKEN);
	sa1111_writel(val, sachip->base + SA1111_SKPCR);
	DPRINTK(KERN_INFO "j720 sa1111 I2S and L3 clocks enabled\n");

	/* Activate and reset the Serial Audio Controller */
	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val |= (SACR0_ENB | SACR0_RST);
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);

	mdelay(5);

	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val &= ~SACR0_RST;
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);
	DPRINTK(KERN_INFO "j720 sa1111 SAC reset and enabled\n");

	/* For I2S, BIT_CLK is supplied internally. The "SA-1111
	 * Specification Update" mentions that the BCKD bit should
	 * be interpreted as "0 = output". Default clock divider
	 * is 22.05kHz.
	 */
	sa1111_sac_writereg(devptr, SACR1_L3EN, SA1111_SACR1);
	DPRINTK(KERN_INFO "j720 sa1111 L3 interface enabled\n");

	// Set samplerate
	sa1111_set_audio_rate(devptr, 22050);
	int rate = sa1111_get_audio_rate(devptr);
	
	spin_unlock(&snd_jornada720_sa1111_sac_lock);

	DPRINTK(KERN_INFO "j720 sa1111 audio samplerate: %d\n", rate);

	DPRINTK(KERN_INFO "done\n");
}