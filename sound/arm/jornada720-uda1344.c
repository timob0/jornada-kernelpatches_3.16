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
#include "jornada720-uda1344.h"

#undef DEBUG
// #define DEBUG

static DEFINE_SPINLOCK(snd_jornada720_sa1111_uda1344_lock);

// The UDA1344 chip instance, initialized with poweron defaults
static struct uda1344 uda_chip = {
	.volume = DEF_VOLUME | DEF_VOLUME << 8,
	.bass   = 50 | 50 << 8,
	.treble = 50 | 50 << 8,
	.line   = 88 | 88 << 8,
	.mic    = 88 | 88 << 8,
	.samplerate = 22050,
	.regs.stat0   = STAT0_SC_512FS | STAT0_IF_LSB16, // <- set i2s interface and 256f/s
	.regs.data0_0 = DATA0_VOLUME(0),
	.regs.data0_1 = DATA1_BASS(0) | DATA1_TREBLE(0),
	.regs.data0_2 = DATA2_DEEMP_NONE | DATA2_FILTER_FLAT,
	.regs.data0_3 = DATA3_POWER_ON,
};

/* Return a reference to the chip instance */
struct uda1344* uda1344_instance(void) {
	return &uda_chip;
}
/* Synchronize registers of the uda_chip instance with the hardware. We need to
 * mirror it in SW since we can't read data from the chip. */ 
static void uda1344_sync(struct sa1111_dev *devptr) {
	struct uda1344 *uda = &uda_chip;
	sa1111_l3_send_byte(devptr, UDA1344_STATUS, STAT0 | uda->regs.stat0);
	sa1111_l3_send_byte(devptr, UDA1344_DATA,   DATA0 | uda->regs.data0_0);
	sa1111_l3_send_byte(devptr, UDA1344_DATA,   DATA1 | uda->regs.data0_1);
	sa1111_l3_send_byte(devptr, UDA1344_DATA,   DATA2 | uda->regs.data0_2);
	sa1111_l3_send_byte(devptr, UDA1344_DATA,   DATA3 | uda->regs.data0_3);
}

/* Initialize the 1344 with some sensible defaults and turn on power. */
int uda1344_open(struct sa1111_dev *devptr) {
	struct uda1344 *uda = &uda_chip;
	uda->active = 1;
	uda->volume = DEF_VOLUME | DEF_VOLUME << 8;
	uda->bass   = 50 | 50 << 8;
	uda->treble = 50 | 50 << 8;
	uda->line   = 88 | 88 << 8;
	uda->mic    = 88 | 88 << 8;
	uda->samplerate = 22050;
	uda->regs.stat0   = STAT0_SC_512FS | STAT0_IF_I2S;
	uda->regs.data0_0 = DATA0_VOLUME(0);
	uda->regs.data0_1 = DATA1_BASS(0) | DATA1_TREBLE(0);
	uda->regs.data0_2 = DATA2_DEEMP_NONE | DATA2_FILTER_FLAT;
	uda->regs.data0_3 = DATA3_POWER_ON;
	uda1344_sync(devptr);
	return 0;
}

/* Close the UDA1344 device, in practice this means we deactivate the power */
void uda1344_close(struct sa1111_dev *devptr) {
	struct uda1344 *uda = &uda_chip;
	uda->active = 0;
	uda->regs.data0_3 = DATA3_POWER_OFF;
	uda1344_sync(devptr);
}

/* Setup the samplerate for both the UDA1344 and the SA1111 devices */
void uda1344_set_samplerate(struct sa1111_dev *devptr, long rate) {
	struct uda1344 *uda = &uda_chip;
	int clk_div = 0;

	/*
	 * We have the following clock sources:
	 * 4.096 MHz, 5.6245 MHz, 11.2896 MHz, 12.288 MHz
	 * Those can be divided either by 256, 384 or 512.
	 * This makes up 12 combinations for the following samplerates...
	 * 
	 * Note: not sure if this is real for Jornada 720 
	 */
	if (rate >= 48000)
		rate = 48000;
	else if (rate >= 44100)
		rate = 44100;
	else if (rate >= 32000)
		rate = 32000;
	else if (rate >= 29400)
		rate = 29400;
	else if (rate >= 24000)
		rate = 24000;
	else if (rate >= 22050)
		rate = 22050;
	else if (rate >= 21970)
		rate = 21970;
	else if (rate >= 16000)
		rate = 16000;
	else if (rate >= 14647)
		rate = 14647;
	else if (rate >= 10985)
		rate = 10985;
	else if (rate >= 10666)
		rate = 10666;
	else
		rate = 8000;
	
	uda->samplerate = rate;

	/* Select the clock divisor */
	uda->regs.stat0 &= ~(STAT0_SC_MASK);
	switch (rate) {
	case 8000:
	case 10985:
	case 22050:
	case 24000:
		uda->regs.stat0 |= STAT0_SC_512FS;
		break;
	case 16000:
	case 21970:
	case 44100:
	case 48000:
		uda->regs.stat0 |= STAT0_SC_256FS;
		break;
	case 10666:
	case 14647:
	case 29400:
	case 32000:
		uda->regs.stat0 |= STAT0_SC_384FS;
		break;
	}

	sa1111_set_audio_rate(devptr, rate);
	uda1344_sync(devptr);
}