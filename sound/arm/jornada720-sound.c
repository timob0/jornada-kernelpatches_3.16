/*
 *  Jornada 720 soundcard
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 *  Copyright (c) by Tomas Kasparek <tomas.kasparek@seznam.cz>
 *  Copyright (c) by Timo Biesenbach <timo.biesenbach@gmail.com>
 * 
 *   This is based on the dummy.c ALSA driver for the generic framework
 *   that is being tailored to the Jornada 720 HPC with the sa1110/1111 
 *   chipset and the UDA1344 sound codec.
 *   From HPs documentation (https://wwwcip.informatik.uni-erlangen.de/~simigern/jornada-7xx/docs/):
 *    3.3. Sound interface
 *     A Philips UDA1344 chip connected to the I2S and L3 port on SA1111.
 *      The speaker is LDD4.
 *      The microphone is LDD3.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *   
 *   History
 *   Oct 10, 2021 - initial version based on sound/driver/dummy.c
 *                                       and sa11xx_uda1341.c
 *
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
#include <linux/dma-mapping.h> 
#include <asm/irq.h>
#include <asm/dma.h>
#include <mach/jornada720.h>
#include <mach/hardware.h>
#include <asm/mach-types.h>
#include <asm/hardware/sa1111.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <sound/info.h>
#include <sound/initval.h>
#include <sound/uda134x.h>

// Sounddriver components
#include "jornada720-common.h"
#include "jornada720-sound.h"
#include "jornada720-sac.h"
#include "jornada720-sacdma.h"
#include "jornada720-uda1344.h"

#ifdef STARTUP_CHIME
#include "octane.h"   // <- 8 bit mono startup sound 11khz (playing too fast)
#endif 

MODULE_AUTHOR("Timo Biesenbach <timo.biesenbach@gmail.com>");
MODULE_DESCRIPTION("Jornada 720 Sound Driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{ALSA,Jornada 720 Sound Driver}}");

// Module specific stuff
static char *id  = SNDRV_DEFAULT_STR1;
static int pcm_substreams = 1;

module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for Jornada 720 UDA1341 soundcard.");

// Sound driver global lock
static DEFINE_SPINLOCK(snd_jornada720_snd_lock);

/*
 * PCM interface
 */
static struct snd_pcm_hardware jornada720_pcm_hardware = {
	.info =				(SNDRV_PCM_INFO_MMAP |
				 		SNDRV_PCM_INFO_INTERLEAVED |				 		
				 		SNDRV_PCM_INFO_MMAP_VALID |
						SNDRV_PCM_INFO_RESUME),
	.formats =			SNDRV_PCM_FMTBIT_S16_LE,
	.rates =			SNDRV_PCM_RATE_22050,
	.rate_min =			22050, // 8000, - clamped to 22khz for now
	.rate_max =			22050,
	.channels_min =		2,
	.channels_max =		2,
	.buffer_bytes_max =	MAX_BUFFER_SIZE,
	.period_bytes_min =	MIN_DMA_BLOCK_SIZE,
	.period_bytes_max =	MAX_DMA_BLOCK_SIZE,
	.periods_min =		2,
	.periods_max =		1024,
	.fifo_size =		0,
};

/*
 *  PCM Stuff
 */

// PCM DMA datastructures
static dma_buf_t playback_buffer;
static dma_buf_t recording_buffer;

/** Debugging aid */
static void dbg_show_buffer(dma_buf_t* buffer) {
	#ifdef DEBUG
	printk(KERN_INFO ">>>>>>>>>>>>>dma_buf_t");
	printk(KERN_INFO ".dma_ptr:        0x%lxh\n", buffer->dma_ptr);
	printk(KERN_INFO ".dma_start:      0x%lxh\n", buffer->dma_start);
	printk(KERN_INFO ".period_size:    0x%lxh\n", buffer->period_size);
	printk(KERN_INFO ".size:           0x%lxh\n", buffer->size);
	printk(KERN_INFO ".virt_addr:      0x%lxh\n", buffer->virt_addr);
	printk(KERN_INFO ".snd_jornada720: 0x%lxh\n", buffer->snd_jornada720);
	printk(KERN_INFO ".loop:           0x%lxh\n", buffer->loop);
	printk(KERN_INFO ".loop count:     0x%lxh\n", buffer->loop_count);
	printk(KERN_INFO "<<<<<<<<<<<<<<<<<<<<");
	#endif
}

/** Called from the DMA interrupt to update the playback position */
static void jornada720_pcm_callback(dma_buf_t *buf, int state) {
	snd_pcm_period_elapsed(buf->snd_jornada720->substream);

	#ifdef DEBUG
	if (state==STATE_RUNNING) {
		printk(KERN_INFO "j720 sa1111 playing");
	}

	if (state==STATE_FINISHED) {
		printk(KERN_INFO "j720 sa1111 stopped");
	} 
	printk(" at adress 0x%lxh, start adress 0x%lxh, size %d bytes\n", buf->dma_ptr, buf->dma_start, buf->size);
	#endif
}

/** Start / Stop PCM playback */
static int jornada720_pcm_trigger(struct snd_pcm_substream *substream, int cmd) {
	struct snd_jornada720 *jornada720 = snd_pcm_substream_chip(substream);
	int err=0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		DPRINTK(KERN_INFO "jornada720_pcm_trigger START / RESUME\n");
		dbg_show_buffer(&playback_buffer);
		err = sa1111_dma_playback(jornada720->pdev_sa1111, &playback_buffer, jornada720_pcm_callback);
		if (err<0) {
			printk(KERN_ERR "sa1111_dma_playback() failed.\n");
			sa1111_dma_playstop(jornada720->pdev_sa1111, &playback_buffer);
		}		
		break;	
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		DPRINTK(KERN_INFO "jornada720_pcm_trigger STOP / SUSPEND\n");
		err = sa1111_dma_playstop(jornada720->pdev_sa1111, &playback_buffer);
		if (err<0) {
			printk(KERN_ERR "sa1111_dma_playstop() failed.\n");
		}
		break;
	default:
		err=-EINVAL;
	}
	return err;
}

/* Prepare for PCM operation */
static int jornada720_pcm_prepare(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_prepare\n");
	struct snd_jornada720 *jornada720 = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	jornada720->substream = substream;

	playback_buffer.dma_ptr = runtime->dma_addr;
	playback_buffer.dma_start = runtime->dma_addr;
	playback_buffer.virt_addr = runtime->dma_area;
	playback_buffer.size = snd_pcm_lib_buffer_bytes(substream);
	playback_buffer.period_size	= snd_pcm_lib_period_bytes(substream);
	playback_buffer.loop = 1;
	dbg_show_buffer(&playback_buffer);

	jornada720->substream = substream;
	return 0;
}

/* Returns the #of frames played so far */
static snd_pcm_uframes_t jornada720_pcm_pointer(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_pointer\n");
	struct snd_pcm_runtime *runtime = substream->runtime;
	ssize_t bytes = playback_buffer.dma_ptr - playback_buffer.dma_start;
	#ifdef DEBUG
	printk(KERN_INFO ".dma_ptr:        0x%lxh\n", playback_buffer.dma_ptr);
	printk(KERN_INFO ".dma_start:      0x%lxh\n", playback_buffer.dma_start);
	printk(KERN_INFO "bytes played %d out of %d\n", bytes, playback_buffer.size);
	#endif
	return bytes_to_frames(runtime, bytes);
}

/* Allocate DMA memory pages */
static int jornada720_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params) {
	DPRINTK(KERN_INFO "jornada720_pcm_hw_params\n");
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

/* Give back the DMA memory pages */
static int jornada720_pcm_hw_free(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_hw_free\n");
	return snd_pcm_lib_free_pages(substream);
}

static int jornada720_pcm_open(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_open\n");
	int err=0;
	struct snd_jornada720 *jornada720 = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	// PCM Open code
	runtime->hw = jornada720_pcm_hardware;

	return 0;
}

static int jornada720_pcm_close(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_close\n");
	int err=0;
	struct snd_jornada720 *jornada720 = snd_pcm_substream_chip(substream);
	// PCM close code

	return 0;
}

static struct snd_pcm_ops jornada720_pcm_ops = {
	.open =		jornada720_pcm_open,
	.close =	jornada720_pcm_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	jornada720_pcm_hw_params,
	.hw_free =	jornada720_pcm_hw_free,
	.prepare =	jornada720_pcm_prepare,
	.trigger =	jornada720_pcm_trigger,
	.pointer =	jornada720_pcm_pointer,
};

/* PCM Destructor */
static void snd_card_jornada720_free(struct snd_jornada720 *jornada720, int device, int substreams) {
	DPRINTK(KERN_INFO "snd_card_jornada720_free\n");
	int err = 0;

	err = sa1111_dma_release(jornada720->pdev_sa1111);
	if (err<0) {
		printk(KERN_ERR "snd_card_jornada720_free: sa1111_dma_release() failed.");	
	}
}

/* PCM Constructor */
static int snd_card_jornada720_pcm(struct snd_jornada720 *jornada720, int device, int substreams) {
	DPRINTK(KERN_INFO "snd_card_jornada720_pcm\n");
	
	struct snd_pcm *pcm;
	struct snd_pcm_ops *ops;
	int err;

	err = snd_pcm_new(jornada720->card, "Jornada720 PCM", device, substreams, substreams, &pcm);
	if (err < 0) return err;

	jornada720->pcm = pcm;
	ops = &jornada720_pcm_ops;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, ops);
	pcm->private_data = jornada720;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Jornada720 PCM");

	// SNDRV_DMA_TYPE_DEV will call alloc_dma_coherent in the end
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV, jornada720->pdev_sa1111, 64*1024, 64*1024);

	// Setup DMA Interrupts
	err = sa1111_dma_alloc(jornada720->pdev_sa1111);
	if (err<0) {
		printk(KERN_ERR "snd_card_jornada720_pcm: sa1111_dma_alloc() failed.");
		return err;
	}

	// Register destructor
	pcm->private_free = snd_card_jornada720_free;

	return 0;
}

/*
 * Mixer interface
 */

#define JORNADA720_VOLUME(xname, xindex, addr) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ, \
  .name = xname, .index = xindex, \
  .info = snd_jornada720_volume_info, \
  .get = snd_jornada720_volume_get, .put = snd_jornada720_volume_put, \
  .private_value = addr, \
  .tlv = { .p = db_scale_jornada720 } }

/* Volume control configuration */
static int snd_jornada720_volume_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo) {
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = -63;
	uinfo->value.integer.max =   0;
	return 0;
}

/* Read volume information from device to userspace */
static int snd_jornada720_volume_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int addr = kcontrol->private_value;

	int left = uda1344_get_volume(jornada720->pdev_sa1111);
	jornada720->mixer_volume[addr][0]=left;
	
	spin_lock_irq(&jornada720->mixer_lock);
	ucontrol->value.integer.value[0] = jornada720->mixer_volume[addr][0];
	// ucontrol->value.integer.value[1] = jornada720->mixer_volume[addr][1];
	spin_unlock_irq(&jornada720->mixer_lock);
	return 0;
}

/* Write volume information from userspace to device */
static int snd_jornada720_volume_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int change, addr = kcontrol->private_value;
	int left; // , right;

	left = ucontrol->value.integer.value[0];
	if (left < -63) left = -63;
	if (left >   0) left =   0;
/*	right = ucontrol->value.integer.value[1];
	if (right < -50) right = -50;
	if (right > 100) right = 100; */
	spin_lock_irq(&jornada720->mixer_lock);
	change = jornada720->mixer_volume[addr][0] != left; /* ||
	         jornada720->mixer_volume[addr][1] != right; */
	jornada720->mixer_volume[addr][0] = left;
//	jornada720->mixer_volume[addr][1] = right;
	spin_unlock_irq(&jornada720->mixer_lock);

	if (change)	uda1344_set_volume(jornada720->pdev_sa1111, left);

	return change;
}

static const DECLARE_TLV_DB_SCALE(db_scale_jornada720, -6300, 100, 0);

#define JORNADA720_CAPSRC(xname, xindex, addr) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = xindex, \
  .info = snd_jornada720_capsrc_info, \
  .get = snd_jornada720_capsrc_get, .put = snd_jornada720_capsrc_put, \
  .private_value = addr }

#define snd_jornada720_capsrc_info	snd_ctl_boolean_stereo_info

static int snd_jornada720_capsrc_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int addr = kcontrol->private_value;

	spin_lock_irq(&jornada720->mixer_lock);
	ucontrol->value.integer.value[0] = jornada720->capture_source[addr][0];
	ucontrol->value.integer.value[1] = jornada720->capture_source[addr][1];
	spin_unlock_irq(&jornada720->mixer_lock);
	return 0;
}

static int snd_jornada720_capsrc_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int change, addr = kcontrol->private_value;
	int left, right;

	left = ucontrol->value.integer.value[0] & 1;
	right = ucontrol->value.integer.value[1] & 1;
	spin_lock_irq(&jornada720->mixer_lock);
	change = jornada720->capture_source[addr][0] != left &&
	         jornada720->capture_source[addr][1] != right;
	jornada720->capture_source[addr][0] = left;
	jornada720->capture_source[addr][1] = right;
	spin_unlock_irq(&jornada720->mixer_lock);
	return change;
}

static struct snd_kcontrol_new snd_jornada720_controls[] = {
	JORNADA720_VOLUME("Master Volume", 0, MIXER_ADDR_MASTER),
	JORNADA720_CAPSRC("Master Capture Switch", 0, MIXER_ADDR_MASTER),
	JORNADA720_VOLUME("Mic Volume", 0, MIXER_ADDR_MIC),
	JORNADA720_CAPSRC("Mic Capture Switch", 0, MIXER_ADDR_MIC),
};

static int snd_card_jornada720_new_mixer(struct snd_jornada720 *jornada720) {
	struct snd_card *card = jornada720->card;
	struct snd_kcontrol *kcontrol;
	unsigned int idx;
	int err;

	spin_lock_init(&jornada720->mixer_lock);
	strcpy(card->mixername, "Jornada 720 Mixer");

	for (idx = 0; idx < ARRAY_SIZE(snd_jornada720_controls); idx++) {
		kcontrol = snd_ctl_new1(&snd_jornada720_controls[idx], jornada720);
		err = snd_ctl_add(card, kcontrol);
		if (err < 0) return err;
	}
	return 0;
}

#if defined(CONFIG_SND_DEBUG) && defined(CONFIG_PROC_FS)
/*
 * proc interface
 */
static void print_formats(struct snd_jornada720 *jornada720, struct snd_info_buffer *buffer) {
	int i;

	for (i = 0; i < SNDRV_PCM_FORMAT_LAST; i++) {
		if (jornada720->pcm_hw.formats & (1ULL << i))
			snd_iprintf(buffer, " %s", snd_pcm_format_name(i));
	}
}

static void print_rates(struct snd_jornada720 *jornada720, struct snd_info_buffer *buffer) {
	static int rates[] = {
	8000,  10666, 10985, 14647,
        16000, 21970, 22050, 24000,
        29400, 32000, 44100, 48000,
	};
	int i;

	if (jornada720->pcm_hw.rates & SNDRV_PCM_RATE_CONTINUOUS) snd_iprintf(buffer, " continuous");
	if (jornada720->pcm_hw.rates & SNDRV_PCM_RATE_KNOT) snd_iprintf(buffer, " knot");
	for (i = 0; i < ARRAY_SIZE(rates); i++)
		if (jornada720->pcm_hw.rates & (1 << i))
			snd_iprintf(buffer, " %d", rates[i]);
}

#define get_jornada720_int_ptr(jornada720, ofs) \
	(unsigned int *)((char *)&((jornada720)->pcm_hw) + (ofs))
#define get_jornada720_ll_ptr(jornada720, ofs) \
	(unsigned long long *)((char *)&((jornada720)->pcm_hw) + (ofs))

struct jornada720_hw_field {
	const char *name;
	const char *format;
	unsigned int offset;
	unsigned int size;
};
#define FIELD_ENTRY(item, fmt) {		   \
	.name = #item,				   \
	.format = fmt,				   \
	.offset = offsetof(struct snd_pcm_hardware, item), \
	.size = sizeof(jornada720_pcm_hardware.item) }

static struct jornada720_hw_field fields[] = {
	FIELD_ENTRY(formats, "%#llx"),
	FIELD_ENTRY(rates, "%#x"),
	FIELD_ENTRY(rate_min, "%d"),
	FIELD_ENTRY(rate_max, "%d"),
	FIELD_ENTRY(channels_min, "%d"),
	FIELD_ENTRY(channels_max, "%d"),
	FIELD_ENTRY(buffer_bytes_max, "%ld"),
	FIELD_ENTRY(period_bytes_min, "%ld"),
	FIELD_ENTRY(period_bytes_max, "%ld"),
	FIELD_ENTRY(periods_min, "%d"),
	FIELD_ENTRY(periods_max, "%d"),
};

static void jornada720_proc_read(struct snd_info_entry *entry, struct snd_info_buffer *buffer) {
	struct snd_jornada720 *jornada720 = entry->private_data;
	int i;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		snd_iprintf(buffer, "%s ", fields[i].name);
		if (fields[i].size == sizeof(int))
			snd_iprintf(buffer, fields[i].format,
				*get_jornada720_int_ptr(jornada720, fields[i].offset));
		else
			snd_iprintf(buffer, fields[i].format,
				*get_jornada720_ll_ptr(jornada720, fields[i].offset));
		if (!strcmp(fields[i].name, "formats"))
			print_formats(jornada720, buffer);
		else if (!strcmp(fields[i].name, "rates"))
			print_rates(jornada720, buffer);
		snd_iprintf(buffer, "\n");
	}
}

static void jornada720_proc_write(struct snd_info_entry *entry, struct snd_info_buffer *buffer) {
	struct snd_jornada720 *jornada720 = entry->private_data;
	char line[64];

	while (!snd_info_get_line(buffer, line, sizeof(line))) {
		char item[20];
		const char *ptr;
		unsigned long long val;
		int i;

		ptr = snd_info_get_str(item, line, sizeof(item));
		for (i = 0; i < ARRAY_SIZE(fields); i++) {
			if (!strcmp(item, fields[i].name))
				break;
		}
		if (i >= ARRAY_SIZE(fields))
			continue;
		snd_info_get_str(item, ptr, sizeof(item));
		if (kstrtoull(item, 0, &val))
			continue;
		if (fields[i].size == sizeof(int))
			*get_jornada720_int_ptr(jornada720, fields[i].offset) = val;
		else
			*get_jornada720_ll_ptr(jornada720, fields[i].offset) = val;
	}
}

static void jornada720_proc_init(struct snd_jornada720 *chip) {
	struct snd_info_entry *entry;

	if (!snd_card_proc_new(chip->card, "jornada720_pcm", &entry)) {
		snd_info_set_text_ops(entry, chip, jornada720_proc_read);
		entry->c.text.write = jornada720_proc_write;
		entry->mode |= S_IWUSR;
		entry->private_data = chip;
	}
}
#else
#define jornada720_proc_init(x)
#endif /* CONFIG_SND_DEBUG && CONFIG_PROC_FS */

// Test hardware setup by playing a sound from hardcoded WAV file
#ifdef STARTUP_CHIME
static void sa1111_play_chime(struct sa1111_dev *devptr) {
	// Clear SAC status register bits 5 & 6 (Tx/Rx FIFO Status)
	unsigned int val = SASCR_ROR | SASCR_TUR;
	sa1111_sac_writereg(devptr, val, SA1111_SASCR);

	// Readout status register
	val = sa1111_sac_readreg(devptr, SA1111_SASR0);
	DPRINTK(KERN_INFO "j720 sa1111 SASR0: 0x%lxh\n", val);

	val = (val >> 8) & 0x0f;
	DPRINTK(KERN_INFO "j720 sa1111 Tx FIFO level: %d\n", val);

	unsigned int i=0;
	unsigned int sample;
	unsigned int sadr;
	s16 left;
	unsigned int round=0;
	while (i < octanestart_wav_len-32) {
		// Simple approach - as long as FIFO not empty, feed it 8 lwords (16bit right / 16bit left)of data

		// check how many elements we can write
		// FIFO fill level is stored in bits 8-11 in SASR0
		// It has 16 elements capacity, however we only can write a burst of 8 at once
		val = sa1111_sac_readreg(devptr, SA1111_SASR0);
		val = (val >> 8) & 0x0F;
		if (val>8) val=8;

		// (8-val)-lword burst write to fill fifo
		for(sadr=0; sadr<(8-val); sadr++) {
			// Shift mono left channel into 32bit sample to l/r channel words
			left  = octanestart_wav[i];
			left  = (left-0x80) << 8; //Convert 8bit unsigned to 16bit signed

			sample  = left & 0x0000FFFF;
			sample  = (sample << 16) & 0xFFFF0000;
			sample  = sample | (left & 0x0000FFFF);
			sa1111_sac_writereg(devptr, sample, SA1111_SADR+(sadr*4));
			i++;
		}

		#ifdef DEBUG
		if (round==0) {
			// Readout status register and fifo levlel
			val = sa1111_sac_readreg(devptr, SA1111_SASR0);
			printk(KERN_DEBUG "j720 sa1111 SASR0: 0x%lxh\n", val);

			val = (val >> 8) & 0x0f;
			printk(KERN_DEBUG "j720 sa1111 Tx FIFO level: %d\n", val);

			printk(KERN_DEBUG "j720 sa1111 Tx left channel 8bit  data: %lx\n", octanestart_wav[i]);
			printk(KERN_DEBUG "j720 sa1111 Tx left channel 16bit data: %lx\n", left);
			printk(KERN_DEBUG "j720 sa1111 Tx sample data            : %lx\n", sample);
			round++;
		}
		#endif

		// Wait until FIFO not full
		do {
			val = sa1111_sac_readreg(devptr, SA1111_SASR0);
		} while((val & SASR0_TNF)==0);
	
		#ifdef DEBUG
		if (round==1) {
			// Readout status register and fifo levlel
			val = sa1111_sac_readreg(devptr, SA1111_SASR0);
			printk(KERN_DEBUG "j720 sa1111 SASR0: 0x%lxh\n", val);

			val = (val >> 8) & 0x0f;
			printk(KERN_DEBUG "j720 sa1111 Tx FIFO level: %d\n", val);

			printk(KERN_DEBUG "j720 sa1111 Tx left channel 8bit  data: %lx\n", octanestart_wav[i]);
			printk(KERN_DEBUG "j720 sa1111 Tx left channel 16bit data: %lx\n", left);
			printk(KERN_DEBUG "j720 sa1111 Tx sample data            : %lx\n", sample);
			round++;
		}
		#endif
	}
}
#endif

/* Here we'll setup all the sound card related stuff 
*  This is called by the sa1111 driver and we get a sa1111_dev struct.
*
*  In here, we need to initialize the hardware so that it is ready
*  to play some sound and register it as a ALSA PCM subsystem.
*  
*  Specifically that means:
*  - Program the SA1111 to use I2S data and L3 control channels
*  - Wake up the UDA1341 chip 
*  - Program SAC for DMA and setup interrupts <- might move this elsewhere
*/
static int snd_jornada720_probe(struct sa1111_dev *devptr) {
	struct snd_card *card;
	struct snd_jornada720 *jornada720;
	struct jornada720_model *m = NULL;
	int idx, err;
	int dev = 0;

	// Let the fun begin. Find the sa1111 chip
	if (!machine_is_jornada720()) {
		printk(KERN_ERR "Jornada 720 soundcard not supported on this hardware\n");
		return -ENODEV;
	}

	// Turn on...
	err = sa1111_enable_device(devptr);
	if (err<0) {
		printk(KERN_ERR "Jornada 720 soundcard could not enable SA1111 SAC device.\n");
		return err;
	}

	// Initialize the SA1111 Serial Audio Controller including I2S and L3 bus
	sa1111_audio_init(devptr);
	
	// Program UDA1344 driver defaults
	err = uda1344_open(devptr);
	if (err < 0) {
		return err;
		printk(KERN_ERR "Jornada 720 soundcard could not initialize UDA1344 Codec\n");
	}

	// Play startup sound
	#ifdef STARTUP_CHIME
	sa1111_play_chime(devptr);
	#endif

	// Register sound card with ALSA subsystem
	err = snd_card_new(&devptr->dev, 0, id, THIS_MODULE, sizeof(struct snd_jornada720), &card);
	if (err < 0) 
		return err;

	jornada720 = card->private_data;
	jornada720->card = card;
	jornada720->pchip_uda1344 = uda1344_instance();
	jornada720->pdev_sa1111 = devptr;

	err = snd_card_jornada720_pcm(jornada720, idx, pcm_substreams);
	if (err < 0)
		goto __nodev;

	jornada720->pcm_hw = jornada720_pcm_hardware;

	err = snd_card_jornada720_new_mixer(jornada720);
	if (err < 0) 
		goto __nodev;

	strcpy(card->driver, "Jornada 720");
	strcpy(card->shortname, "Jornada 720");
	sprintf(card->longname, "Jornada 720 %i", dev + 1);

	jornada720_proc_init(jornada720);
	// Setup buffers
	playback_buffer.snd_jornada720 = jornada720;
	recording_buffer.snd_jornada720 = jornada720;

	err = snd_card_register(card);
	if (err == 0) {
		sa1111_set_drvdata(devptr, card);
		return 0;
	}
      __nodev:
	snd_card_free(card);
	return err;
}

/* Counterpart to probe(), shutdown stuff here that was initialized in probe() */
static int snd_jornada720_remove(struct sa1111_dev *devptr) {
	// Close Codec
	uda1344_close(devptr);

	// Turn SAC off
	sa1111_disable_device(devptr);

	snd_card_free(sa1111_get_drvdata(devptr));
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int snd_jornada720_suspend(struct device *pdev)
{
	struct snd_card *card = dev_get_drvdata(pdev);
	struct snd_jornada720 *jornada720 = card->private_data;

	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	snd_pcm_suspend_all(jornada720->pcm);
	return 0;
}
	
static int snd_jornada720_resume(struct device *pdev)
{
	struct snd_card *card = dev_get_drvdata(pdev);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}

static SIMPLE_DEV_PM_OPS(snd_jornada720_pm, snd_jornada720_suspend, snd_jornada720_resume);
#define SND_JORNADA720_PM_OPS	&snd_jornada720_pm
#else
#define SND_JORNADA720_PM_OPS	NULL
#endif

#define SND_JORNADA720_DRIVER	"snd_jornada720"
static struct sa1111_driver snd_jornada720_driver = {
        .drv = {
                .name   = SND_JORNADA720_DRIVER,
                .owner  = THIS_MODULE,
        },
        .devid          = SA1111_DEVID_SAC,
        .probe          = snd_jornada720_probe,
        .remove         = snd_jornada720_remove,
};


static void snd_jornada720_unregister_all(void) {
	sa1111_driver_unregister(&snd_jornada720_driver);
}

/** This is the module init, it has no conception of what we are yet, i.e. other than the name implies it does
 *  not know that we're a soundcard. Therefore the job of this portion is to register ourselves with the
 *  "bus" we're hanging off (sa1111) so that we gain access to that.
 * 
 *  Then, we'll create a soundcard device and register it.
 */
static int __init alsa_card_jornada720_init(void)
{
	int err;
	err = sa1111_driver_register(&snd_jornada720_driver);
	if (err < 0)
		return err;
	
	return 0;
}

/* Module exit, do cleanup work here.
 */
static void __exit alsa_card_jornada720_exit(void)
{
	snd_jornada720_unregister_all();
}

module_init(alsa_card_jornada720_init)
module_exit(alsa_card_jornada720_exit)