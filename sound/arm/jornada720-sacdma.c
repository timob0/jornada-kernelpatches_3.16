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

#define DEBUG
#ifdef DEBUG
#define DPRINTK( s, arg... )  printk( "dma<%s>: " s, dma->device_id , ##arg )
#else
#define DPRINTK( x... )
#endif

/* Our DMA channels */
sa1100_dma_t dma_chan[SA1111_SAC_DMA_CHANNELS];

/*
 * Control register structure for the SA1111 SAC DMA
 */
void sa1111_reset_sac_dma(struct sa1111_dev *devptr, dmach_t channel) {
	sa1100_dma_t *dma = &dma_chan[channel];
	dma->regs->SAD_CS = 0;
	mdelay(1);
	dma->dma_a = dma->dma_b = 0;
}

int start_sa1111_sac_dma(struct sa1111_dev *devptr, sa1100_dma_t *dma, dma_addr_t dma_ptr, size_t size) {
  	dma_regs_t *sac_regs = dma->regs;
	DPRINTK(" SAC DMA %cCS %02x at %08x (%d)\n", (sac_regs==SA1111_SADTCS)?'T':'R', sac_regs->SAD_CS, dma_ptr, size);

	#ifdef DEBUG
	//Useless warning
	if( size < SA1111_SAC_DMA_MIN_XFER ) printk(KERN_ERR "Warning: SAC xfers below %u bytes may be buggy! (%u bytes)\n", SA1111_SAC_DMA_MIN_XFER, size);
	#endif

	if( dma->dma_a && dma->dma_b ){
	  	DPRINTK("  neither engine available! (A %d, B %d)\n",
			dma->dma_a, dma->dma_b);
	  	return -1;
	}
	
	// Useless warning?
	#ifdef DEBUG
	if( sa1111_check_dma_bug(dma_ptr) ) printk(KERN_ERR "Warning: DMA address %08x is buggy!\n", dma_ptr);
	#endif

	if( (dma->last_dma || dma->dma_b) && dma->dma_a == 0 ){
	  	if( sac_regs->SAD_CS & SAD_CS_DBDB ){
		  	DPRINTK("  awaiting \"done B\" interrupt, not starting\n");
			return -1;
		}
		sac_regs->SAD_SA = SA1111_DMA_ADDR((u_int)dma_ptr);
		sac_regs->SAD_CA = size;
		sac_regs->SAD_CS = SAD_CS_DSTA | SAD_CS_DEN;
		++dma->dma_a;
		DPRINTK("  with A [%02lx %08lx %04lx]\n", sac_regs->SAD_CS,
			sac_regs->SAD_SA, sac_regs->SAD_CA);
	} else {
	  	if( sac_regs->SAD_CS & SAD_CS_DBDA) { 
			DPRINTK("  awaiting \"done A\" interrupt, not starting\n");
			return -1;
		}
		sac_regs->SAD_SB = SA1111_DMA_ADDR((u_int)dma_ptr);
		sac_regs->SAD_CB = size;
		sac_regs->SAD_CS = SAD_CS_DSTB | SAD_CS_DEN;
		++dma->dma_b;
		DPRINTK("  with B [%02lx %08lx %04lx]\n", sac_regs->SAD_CS,	sac_regs->SAD_SB, sac_regs->SAD_CB);
	}
	/* Additional delay to avoid DMA engine lockup during record: */
	// if( sac_regs == (dma_regs_t*)&SA1111_SADRCS )
	//  	mdelay(1);	/* NP : wouuuh! ugly... */
	return 0;
}

static void sa1111_sac_dma_irq(struct sa1111_dev *devptr, int irq, void *dev_id, struct pt_regs *regs) {
  	sa1100_dma_t *dma = (sa1100_dma_t *) dev_id;
	DPRINTK("irq %d, last DMA serviced was %c, CS %02x\n", irq,
		dma->last_dma?'B':'A', dma->regs->SAD_CS);
	/* Occasionally, one of the DMA engines (A or B) will
	 * lock up. We try to deal with this by quietly kicking
	 * the control register for the afflicted transfer
	 * direction.
	 *
	 * Note for the debugging-inclined: we currently aren't
	 * properly flushing the DMA engines during channel
	 * shutdown. A slight hiccup at the beginning of playback
	 * after a channel has been stopped can be heard as
	 * evidence of this. Programmatically, this shows up
	 * as either a locked engine, or a spurious interrupt. -jd
	 */
	if(irq==AUDXMTDMADONEA || irq==AUDRCVDMADONEA){
	  	if(dma->last_dma == 0){
		  	DPRINTK("DMA B has locked up!\n");
			dma->regs->SAD_CS = 0;
			mdelay(1);
			dma->dma_a = dma->dma_b = 0;
		} else {
		  	if(dma->dma_a == 0)
			  	DPRINTK("spurious SAC IRQ %d\n", irq);
			else {
			  	--dma->dma_a;
				/* Servicing the SAC DMA engines too quickly
				 * after they issue a DONE interrupt causes
				 * them to lock up.
				 */
				if(irq==AUDRCVDMADONEA || irq==AUDRCVDMADONEB)
				  	mdelay(1);
			}
		}
		dma->regs->SAD_CS = SAD_CS_DBDA | SAD_CS_DEN; /* w1c */
		dma->last_dma = 0;
	} else {
	  	if(dma->last_dma == 1){
		  	DPRINTK("DMA A has locked up!\n");
			dma->regs->SAD_CS = 0;
			mdelay(1);
			dma->dma_a = dma->dma_b = 0;
		} else {
		  	if(dma->dma_b == 0)
			  	DPRINTK("spurious SAC IRQ %d\n", irq);
			else {
			  	--dma->dma_b;
				/* See lock-up note above. */
				if(irq==AUDRCVDMADONEA || irq==AUDRCVDMADONEB)
				  	mdelay(1);
			}
		}
		dma->regs->SAD_CS = SAD_CS_DBDB | SAD_CS_DEN; /* w1c */
		dma->last_dma = 1;
	}
	/* NP: maybe this shouldn't be called in all cases? */
	// sa1100_dma_done (dma);
}

int sa1111_sac_request_dma(struct sa1111_dev *devptr, dmach_t *channel, const char *device_id, unsigned int direction) {
	sa1100_dma_t *dma = NULL;
	int ch, irq, err;
	*channel = -1;		/* to be sure we catch the freeing of a misregistered channel */
	ch = SA1111_SAC_DMA_BASE + direction;
	if (!channel_is_sa1111_sac(ch)) {
	  	printk(KERN_ERR "%s: invalid SA-1111 SAC DMA channel (%d)\n", device_id, ch);
		return -1;
	}
	dma = &dma_chan[ch];
	if (xchg(&dma->in_use, 1) == 1) {
	  	printk(KERN_ERR "%s: SA-1111 SAC DMA channel %d in use\n", device_id, ch);
		return -EBUSY;
	}
	irq = AUDXMTDMADONEA + direction;
	err = request_irq(irq, sa1111_sac_dma_irq, IRQF_TRIGGER_RISING, device_id, (void *) dma);
	if (err) {
		printk(KERN_ERR "%s: unable to request IRQ %d for DMA channel %d (A)\n", device_id, irq, ch);
		dma->in_use = 0;
		return err;
	}
	irq = AUDXMTDMADONEB + direction;
	err = request_irq(irq, sa1111_sac_dma_irq, IRQF_TRIGGER_RISING, device_id, (void *) dma);
	if (err) {
		printk(KERN_ERR "%s: unable to request IRQ %d for DMA channel %d (B)\n", device_id, irq, ch);
		dma->in_use = 0;
		return err;
	}
	*channel = ch;
	dma->device_id = device_id;
	dma->callback = NULL;
	dma->spin_size = 0;
	return 0;
}

/* FIXME:  need to complete the three following functions */
int sa1111_dma_get_current(struct sa1111_dev *devptr, dmach_t channel, void **buf_id, dma_addr_t *addr) {
	sa1100_dma_t *dma = &dma_chan[channel];
	int flags, ret;
	local_irq_save(flags);
	if (dma->curr && dma->spin_ref <= 0) {
		dma_buf_t *buf = dma->curr;
		if (buf_id)
			*buf_id = buf->id;
		/* not fully accurate but still... */
		*addr = buf->dma_ptr;
		ret = 0;
	} else {
		if (buf_id)
			*buf_id = NULL;
		*addr = 0;
		ret = -ENXIO;
	}
	local_irq_restore(flags);
	return ret;
}
int sa1111_dma_stop(struct sa1111_dev *devptr, dmach_t channel) {
	return 0;
}
int sa1111_dma_resume(struct sa1111_dev *devptr, dmach_t channel) {
	return 0;
}
void sa1111_cleanup_sac_dma(struct sa1111_dev *devptr, dmach_t channel) {
	sa1100_dma_t *dma = &dma_chan[channel];
	free_irq(AUDXMTDMADONEA + (channel - SA1111_SAC_DMA_BASE), (void*) dma);
	free_irq(AUDXMTDMADONEB + (channel - SA1111_SAC_DMA_BASE), (void*) dma);
}

static int sa1111_init_sac_dma(struct sa1111_dev *devptr)
{
	int channel = SA1111_SAC_DMA_BASE;
	dma_chan[channel++].regs = (dma_regs_t *) SA1111_SADTCS;
	dma_chan[channel++].regs = (dma_regs_t *) SA1111_SADTCS;

	return 0;
}