/*
 * Definitions shared between dma-sa1100.c and dma-sa1111.c
 * (C) 2000 Nicolas Pitre <nico@cam.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
// #include <linux/config.h>
#ifndef JORNADA720_SACDMA_H
#define JORNADA720_SACDMA_H

#define SA1111_SAC_DMA_CHANNELS 2
#define SA1111_SAC_XMT_CHANNEL  0
#define SA1111_SAC_RCV_CHANNEL  1

#define SA1111_SAC_DMA_BASE 0

#define DMA_DIR_OUT 0
#define DMA_DIR_IN  1

#define DMA_REG_RX_OFS 0x14
#define DMA_CH_A   0x00
#define DMA_CH_B   0x08

typedef unsigned int dma_device_t;
typedef unsigned int dma_addr_t;
typedef unsigned int dmach_t;

typedef void (*dma_callback_t)( void *buf_id, int size );

typedef struct {
	volatile unsigned int SAD_CS;
	volatile dma_addr_t SAD_SA;
	volatile unsigned int SAD_CA;
	volatile dma_addr_t SAD_SB;
	volatile unsigned int SAD_CB;
} dma_regs_t;

typedef struct dma_buf_s {
	int size;		/* buffer size */
	dma_addr_t dma_start;	/* starting DMA address */
	dma_addr_t dma_ptr;	/* next DMA pointer to use */
	int ref;		/* number of DMA references */
	void *id;		/* to identify buffer from outside */
	struct dma_buf_s *next;	/* next buffer to process */
} dma_buf_t;
/*
 * DMA channel structure.
 */
typedef struct {
	unsigned int in_use;	/* Device is allocated */
	const char *device_id;	/* Device name */
	dma_device_t device;	/* ... to which this channel is attached */
	dma_buf_t *head;		/* where to insert buffers */
	dma_buf_t *tail;		/* where to remove buffers */
	dma_buf_t *curr;		/* buffer currently DMA'ed */
	int stopped;			/* 1 if DMA is stalled */
	dma_regs_t *regs;		/* points to appropriate DMA registers */
	int irq;				/* IRQ used by the channel */
	dma_callback_t callback; /* ... to call when buffers are done */
	int spin_size;			/* > 0 when DMA should spin when no more buffer */
	dma_addr_t spin_addr;	/* DMA address to spin onto */
	int spin_ref;			/* number of spinning references */
	int dma_a, dma_b, last_dma; /* SA-1111 specific */
} sa1100_dma_t;

extern void sa1111_reset_sac_dma(struct sa1111_dev *devptr);
extern int start_sa1111_sac_dma(struct sa1111_dev *devptr, dma_addr_t dma_ptr, size_t size, int direction);
extern int done_sa1111_sac_dma(struct sa1111_dev *devptr, int direction);

int sa1111_dma_get_current(struct sa1111_dev *devptr, dmach_t channel, void **buf_id, dma_addr_t *addr);
int sa1111_dma_stop(struct sa1111_dev *devptr, dmach_t channel);
int sa1111_dma_resume(struct sa1111_dev *devptr, dmach_t channel);
void sa1111_cleanup_sac_dma(struct sa1111_dev *devptr, dmach_t channel);

#ifdef CONFIG_SA1111
#define channel_is_sa1111_sac(ch) \
	((ch) >= SA1111_SAC_DMA_BASE && \
	 (ch) <  SA1111_SAC_DMA_BASE + SA1111_SAC_DMA_CHANNELS)
#else
#define channel_is_sa1111_sac(ch) (0)
#endif

// From top ifndef
#endif