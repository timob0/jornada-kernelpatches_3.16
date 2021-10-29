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

// Testsound for debugging the codec remove later!
#include "octane.h"   // <- 8 bit mono startup sound 11khz
#include "pcm1622.h"  // <- 16bit stereo sound 22 khz
#include "jornada720.h"
#include "jornada720-sacdma.h"

// Debugging aid
#undef DEBUG
#define DEBUG
#ifdef DEBUG
#define DPRINTK(msg) printk(msg)
#else
#define DPRINTK(msg)
#endif

MODULE_AUTHOR("Timo Biesenbach <timo.biesenbach@gmail.com>");
MODULE_DESCRIPTION("Jornada 720 Sound Driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{ALSA,Jornada 720 Sound Driver}}");

// Module specific stuff
static char *id  = SNDRV_DEFAULT_STR1;
static bool enable = SNDRV_DEFAULT_ENABLE1;
static char *model = UDA1344_NAME;
static int pcm_devs = 1;
static int pcm_substreams = 1;
static bool fake_buffer = 1;

module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for Jornada 720 UDA1341 soundcard.");

// Lock 
static DEFINE_SPINLOCK(snd_jornada720_sa1111_lock);

// Our Device
static struct platform_device *device;

// The UDA1344 chip instance
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

/*
 * Card model
 */
struct jornada720_model model_uda1344 = {
	.name = "uda1344",
	.buffer_bytes_max = 16380,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min = 2,
	.channels_max = 2,
	.periods_min = 2,
	.periods_max = 255,
};

// SA1111 Sound Controller interface
static inline void         sa1111_sac_writereg(struct sa1111_dev *devptr, unsigned int val, u32 reg) {
	sa1111_writel(val, devptr->mapbase + reg);
}

static inline unsigned int sa1111_sac_readreg(struct sa1111_dev *devptr, u32 reg) {
	return sa1111_readl(devptr->mapbase + reg);
}

// Send bytes via SA1111-L3
static inline void 		   sa1111_l3_send_byte(struct sa1111_dev *devptr, unsigned char addr, unsigned char dat) {
	int i=0;
	unsigned int SASCR;
	unsigned int SACR1;
	
	// Make sure only one thread is in the critical section below.
	spin_lock(&snd_jornada720_sa1111_lock);
	
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
		/*
		SACR1 = sa1111_sac_readreg(devptr, SA1111_SACR1);
		SACR1 &= ~SACR1_L3EN;
		sa1111_sac_writereg(devptr, SACR1, SA1111_SACR1);

		mdelay(100);

		SACR1 = sa1111_sac_readreg(devptr, SA1111_SACR1);
		SACR1 |= SACR1_L3EN;
		sa1111_sac_writereg(devptr, SACR1, SA1111_SACR1);
		*/
	}
	
	SASCR = SASCR_DTS|SASCR_RDD;
	sa1111_sac_writereg(devptr, SASCR, SA1111_SASCR);
	
	// Give up the lock
	spin_unlock(&snd_jornada720_sa1111_lock);
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
static int uda1344_open(struct sa1111_dev *devptr) {
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
static void uda1344_close(struct sa1111_dev *devptr) {
	struct uda1344 *uda = &uda_chip;
	uda->active = 0;
	uda->regs.data0_3 = DATA3_POWER_OFF;
	uda1344_sync(devptr);
}

/* Setup the samplerate for both the UDA1344 and the SA1111 devices */
static void uda1344_set_samplerate(struct sa1111_dev *devptr, long rate) {
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

/*
 * PCM interface
 */

/** Start / Stop PCM playback */
/* In reality calls timer_ops_ runtime private data */
static int jornada720_pcm_trigger(struct snd_pcm_substream *substream, int cmd) {
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		DPRINTK(KERN_INFO "jornada720_pcm_trigger START / RESUME\n");	
		return get_jornada720_ops(substream)->start(substream);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		DPRINTK(KERN_INFO "jornada720_pcm_trigger STOP / SUSPEND\n");
		return get_jornada720_ops(substream)->stop(substream);
	}
	return -EINVAL;
}

/* In reality calls timer_ops_ runtime private data */
static int jornada720_pcm_prepare(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_prepare\n");	
	return get_jornada720_ops(substream)->prepare(substream);
}

/* In reality calls timer_ops_ runtime private data */
static snd_pcm_uframes_t jornada720_pcm_pointer(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_pointer\n");	
	return get_jornada720_ops(substream)->pointer(substream);
}

static struct snd_pcm_hardware jornada720_pcm_hardware = {
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_MMAP_VALID),
	.formats =		USE_FORMATS,
	.rates =		USE_RATE,
	.rate_min =		USE_RATE_MIN,
	.rate_max =		USE_RATE_MAX,
	.channels_min =		USE_CHANNELS_MIN,
	.channels_max =		USE_CHANNELS_MAX,
	.buffer_bytes_max =	MAX_BUFFER_SIZE,
	.period_bytes_min =	MIN_PERIOD_SIZE,
	.period_bytes_max =	MAX_PERIOD_SIZE,
	.periods_min =		USE_PERIODS_MIN,
	.periods_max =		USE_PERIODS_MAX,
	.fifo_size =		0,
};

/* Allocate DMA memory pages */
static int jornada720_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params) {
	DPRINTK(KERN_INFO "jornada720_pcm_hw_params\n");
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int jornada720_pcm_hw_free(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_hw_free\n");		
	return snd_pcm_lib_free_pages(substream);
}

static int jornada720_pcm_open(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_open\n");	
	struct snd_jornada720 *jornada720 = snd_pcm_substream_chip(substream);
	struct jornada720_model *model = jornada720->model;
	struct snd_pcm_runtime *runtime = substream->runtime;
	const struct jornada720_timer_ops *ops;
	int err;

	err = ops->create(substream);
	if (err < 0) return err;

	get_jornada720_ops(substream) = ops;

	runtime->hw = jornada720->pcm_hw;
	if (substream->pcm->device & 1) {
		runtime->hw.info &= ~SNDRV_PCM_INFO_INTERLEAVED;
		runtime->hw.info |= SNDRV_PCM_INFO_NONINTERLEAVED;
	}
	if (substream->pcm->device & 2)
		runtime->hw.info &= ~(SNDRV_PCM_INFO_MMAP |
				      SNDRV_PCM_INFO_MMAP_VALID);

	if (model == NULL) return 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (model->playback_constraints) err = model->playback_constraints(substream->runtime);
	} else {
		if (model->capture_constraints) err = model->capture_constraints(substream->runtime);
	}
	if (err < 0) {
		get_jornada720_ops(substream)->free(substream);
		return err;
	}
	return 0;
}

static int jornada720_pcm_close(struct snd_pcm_substream *substream) {
	DPRINTK(KERN_INFO "jornada720_pcm_close\n");
	get_jornada720_ops(substream)->free(substream);
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

/* Initialize the J720 pcm playback buffers */
static int snd_card_jornada720_pcm(struct snd_jornada720 *jornada720, int device, int substreams) {
	struct snd_pcm *pcm;
	struct snd_pcm_ops *ops;
	int err;
	DPRINTK(KERN_INFO "snd_card_jornada720_pcm\n");
	err = snd_pcm_new(jornada720->card, "Jornada720 PCM", device, substreams, substreams, &pcm);
	if (err < 0) return err;

	jornada720->pcm = pcm;
	ops = &jornada720_pcm_ops;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, ops);
	pcm->private_data = jornada720;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Jornada720 PCM");


	snd_pcm_lib_preallocate_pages_for_all(pcm,
		SNDRV_DMA_TYPE_CONTINUOUS,
		snd_dma_continuous_data(GFP_KERNEL),
		0, 64*1024);
	return 0;
}

/*
 * mixer interface
 */

#define JORNADA720_VOLUME(xname, xindex, addr) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, \
  .access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ, \
  .name = xname, .index = xindex, \
  .info = snd_jornada720_volume_info, \
  .get = snd_jornada720_volume_get, .put = snd_jornada720_volume_put, \
  .private_value = addr, \
  .tlv = { .p = db_scale_jornada720 } }

static int snd_jornada720_volume_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo) {
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = -50;
	uinfo->value.integer.max = 100;
	return 0;
}
 
static int snd_jornada720_volume_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int addr = kcontrol->private_value;

	spin_lock_irq(&jornada720->mixer_lock);
	ucontrol->value.integer.value[0] = jornada720->mixer_volume[addr][0];
	ucontrol->value.integer.value[1] = jornada720->mixer_volume[addr][1];
	spin_unlock_irq(&jornada720->mixer_lock);
	return 0;
}

static int snd_jornada720_volume_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int change, addr = kcontrol->private_value;
	int left, right;

	left = ucontrol->value.integer.value[0];
	if (left < -50) left = -50;
	if (left > 100) left = 100;
	right = ucontrol->value.integer.value[1];
	if (right < -50) right = -50;
	if (right > 100) right = 100;
	spin_lock_irq(&jornada720->mixer_lock);
	change = jornada720->mixer_volume[addr][0] != left ||
	         jornada720->mixer_volume[addr][1] != right;
	jornada720->mixer_volume[addr][0] = left;
	jornada720->mixer_volume[addr][1] = right;
	spin_unlock_irq(&jornada720->mixer_lock);
	return change;
}

static const DECLARE_TLV_DB_SCALE(db_scale_jornada720, -4500, 30, 0);

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


/* Simplistic handler routine to test the SA1111 Audio DMA Done interrupts. */
static void sa1111_test_irqhandler(int irq, void *dev_id, struct pt_regs *regs)  {
	printk(KERN_ERR "sa1111_test_irqhandler\n");
	switch (irq) {
		case AUDXMTDMADONEA: printk("sa1111_test_irqhandler: AUDXMTDMADONEA\n"); break;
		case AUDXMTDMADONEB: printk("sa1111_test_irqhandler: AUDXMTDMADONEB\n"); break;
		case AUDRCVDMADONEA: printk("sa1111_test_irqhandler: AUDRCVDMADONEA\n"); break;
		case AUDRCVDMADONEB: printk("sa1111_test_irqhandler: AUDXMTDMADONEB\n"); break;
	}
}

/* Setup interrupt handling for the transfer completion events from SA1111
 * Note: request_irq will enable the interrupt handling. We need to tell the
 * chip to generate them though. 
 */
static int sa1111_test_irqrequest(struct sa1111_dev *devptr, unsigned int direction) {
	printk(KERN_ERR "sa1111_test_irqrequest\n");
	int ch, irqa, irqb, err;

	// direction: 0 play, 1 record
	irqa = AUDXMTDMADONEA + direction;
	err = request_irq(irqa, sa1111_test_irqhandler, IRQF_TRIGGER_RISING, SA1111_DRIVER_NAME(devptr), sa1111_get_drvdata(devptr));
	if (err) {
		printk(KERN_ERR "unable to request IRQ %d for DMA channel %d (A)\n", irqa, ch);
		return err;
	}

	irqb = AUDXMTDMADONEB + direction;
	err = request_irq(irqb, sa1111_test_irqhandler, IRQF_TRIGGER_RISING, SA1111_DRIVER_NAME(devptr), sa1111_get_drvdata(devptr));
	if (err) {
		printk(KERN_ERR "unable to request IRQ %d for DMA channel %d (B)\n", irqb, ch);
		return err;
	}	
	return 0;
}

/* Release the IRQ handler */
static void sa1111_test_irqrelease(struct sa1111_dev *devptr, unsigned int direction) {
	int irqa, irqb;
	printk(KERN_ERR "sa1111_test_irqrelease\n");

	irqa = AUDXMTDMADONEA + direction;
	irqb = AUDXMTDMADONEB + direction;

	free_irq(irqa, sa1111_get_drvdata(devptr));
	free_irq(irqb, sa1111_get_drvdata(devptr));
}

/* DMA test routine for the SA1111 SAC. Will replay a hardcoded ~500kb 16bit 22khz stereo sample.*/
static void sa1111_test_dma(struct sa1111_dev *devptr) { 
	dmach_t channel;

	printk(KERN_INFO "j720 sa1111 Init DMA registes.\n");
	sa1111_reset_sac_dma(devptr);

	//Double buffer
	dma_addr_t dma_phys_addr[2]; // <-- phys address
	void *     dma_virt_addr[2]; // <-- virtual address

	const unsigned int dma_block_size = (1<<12); // 4kb buffer block

	// Setup IRQ Handler....
	printk(KERN_INFO "j720 sa1111 Request interrupts\n");
	int err = sa1111_test_irqrequest(devptr, DMA_DIR_OUT);
	if (err<0) {
		printk(KERN_ERR "j720 sa1111 Could not setup IRQ handler, exiting!\n");
		goto out;
	}

	printk(KERN_INFO "j720 sa1111 Allocate 4kb DMA memory buffer\n");
	dma_virt_addr[0] = dma_alloc_coherent(devptr, dma_block_size, &dma_phys_addr[0], 0);
	dma_virt_addr[1] = dma_alloc_coherent(devptr, dma_block_size, &dma_phys_addr[1], 0);

	if (dma_virt_addr[0]!=NULL && dma_virt_addr[1]!=NULL) {
		char* sample = sample = &pcm1622s_wav[0];
		int cnt=0;

		// Init memory buffer #1 for first playback
		printk(KERN_INFO "j720 sa1111 Copy data to DMA memory\n");
		memcpy(dma_virt_addr[cnt%2], sample, dma_block_size);

		// Loop to replay the whole sample in 8kb iterations
		for (sample = &pcm1622s_wav[0]; sample < &pcm1622s_wav[pcm1622s_wav_len] - (dma_block_size*2); sample+=dma_block_size) {

			// Start playing the sound using DMA transfer
			#ifdef DEBUG
			printk(KERN_DEBUG "j720 sa1111 Starting SAC DMA for playback from src address 0x%lxh.\n", sample);
			#endif

			int err = start_sa1111_sac_dma(devptr, dma_phys_addr[cnt%2], dma_block_size, DMA_DIR_OUT);
			if (err<0) {
				printk(KERN_ERR "j720 sa1111 Start DMA failed, terminating!\n");
				break; // <- exit loop	
			}
			cnt++;

			// Copy to allocated memory buffer
			#ifdef DEBUG
			printk(KERN_DEBUG "j720 sa1111 Copy data to DMA memory\n");
			#endif
			
			memcpy(dma_virt_addr[cnt%2], sample, dma_block_size);

			// Wait for transfer to complete
			while(!done_sa1111_sac_dma(devptr, DMA_DIR_OUT)) {
				udelay(1);
			}
		}
		// Free memory
		printk(KERN_INFO "j720 sa1111 Release DMA memory\n");
		dma_free_coherent(devptr, dma_block_size, dma_virt_addr[0], dma_virt_addr[0]);
		dma_free_coherent(devptr, dma_block_size, dma_virt_addr[1], dma_virt_addr[1]);
	} else {
			printk(KERN_ERR "j720 sa1111 Could not allocate DMA memory!\n");
	}

 out:
	printk(KERN_INFO "j720 sa1111 Release IRQs\n"); 	
    sa1111_test_irqrelease(devptr, DMA_DIR_OUT);
 	printk(KERN_INFO "j720 sa1111 sa1111_test_dma end.\n");
}

// Test hardware setup by playing a sound from hardcoded WAV file
static void sa1111_audio_test(struct sa1111_dev *devptr) {
	// Clear SAC status register bits 5 & 6 (Tx/Rx FIFO Status)
	unsigned int val = SASCR_ROR | SASCR_TUR;
	sa1111_sac_writereg(devptr, val, SA1111_SASCR);

	// Readout status register
	val = sa1111_sac_readreg(devptr, SA1111_SASR0);
	printk(KERN_INFO "j720 sa1111 SASR0: 0x%lxh\n", val);

	val = (val >> 8) & 0x0f;
	printk(KERN_INFO "j720 sa1111 Tx FIFO level: %d\n", val);

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

/*
 *	Get the parent device driver structure from a child function device
 *  Copied here since unfortunately not exported by sa1111.h
 */
struct sa1111 {
	struct device	*dev;
	struct clk	*clk;
	unsigned long	phys;
	int		irq;
	int		irq_base;	/* base for cascaded on-chip IRQs */
	spinlock_t	lock;
	void __iomem	*base;
	struct sa1111_platform_data *pdata;
#ifdef CONFIG_PM
	void		*saved_state;
#endif
};

static inline struct sa1111 *sa1111_chip_driver(struct sa1111_dev *sadev)
{
	return (struct sa1111 *)dev_get_drvdata(sadev->dev.parent);
}

// Copied from "glue audio driver" and heavily modified
// Will initialize the SA1111 and its L3 hardware
static void sa1111_audio_init(struct sa1111_dev *devptr) {
	// For register bitbanging
	unsigned int val; 

	// Get access to the "parent" sa1111 chip 
	struct sa1111 *sachip = sa1111_chip_driver(devptr);

	printk(KERN_INFO "j720 sa1111 init...");
	printk(KERN_INFO "j720 sa1111 device id: %d\n", devptr->devid);
	printk(KERN_INFO "j720 sa1111 chip base: 0x%lxh\n", sachip->base);
	printk(KERN_INFO "j720 sa1111 SAC  base: 0x%lxh\n", devptr->mapbase);

	PPSR &= ~(PPC_LDD3 | PPC_LDD4);
	PPDR |= PPC_LDD3 | PPC_LDD4;
	PPSR |= PPC_LDD4; /* enable speaker */
	PPSR |= PPC_LDD3; /* enable microphone */
	printk(KERN_INFO "j720 sa1111 speaker/mic pre-amps enabled\n");
	
	// deselect AC Link
	sa1111_select_audio_mode(devptr, SA1111_AUDIO_I2S);
	printk(KERN_INFO "j720 sa1111 I2S protocol enabled\n");

	/* Enable the I2S clock and L3 bus clock. This is a function in another SA1111 block
	 * which is why we need the sachip stuff (should probably be a function in sa1111.c/h)
	 */
	val = sa1111_readl(sachip->base + SA1111_SKPCR);
	val|= (SKPCR_I2SCLKEN | SKPCR_L3CLKEN);
	sa1111_writel(val, sachip->base + SA1111_SKPCR);
	printk(KERN_INFO "j720 sa1111 I2S and L3 clocks enabled\n");

	/* Activate and reset the Serial Audio Controller */
	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val |= (SACR0_ENB | SACR0_RST);
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);
	mdelay(5);
	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val &= ~SACR0_RST;
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);
	printk(KERN_INFO "j720 sa1111 SAC reset and enabled\n");

	/* For I2S, BIT_CLK is supplied internally. The "SA-1111
	 * Specification Update" mentions that the BCKD bit should
	 * be interpreted as "0 = output". Default clock divider
	 * is 22.05kHz.
	 */
	sa1111_sac_writereg(devptr, SACR1_L3EN, SA1111_SACR1);
	printk(KERN_INFO "j720 sa1111 L3 interface enabled\n");

	// Set samplerate
	sa1111_set_audio_rate(devptr, 22050);
	int rate = sa1111_get_audio_rate(devptr);
	printk(KERN_INFO "j720 sa1111 audio samplerate: %d\n", rate);

	printk(KERN_INFO "done\n");
}

/* Here we'll setup all the sound card related stuff 
*  This is called by the sa1111 driver and we get a sa1111_dev struct.
*
*  In here, we need to initialize the hardware so that it is ready
*  to play some sound and register it as a ALSA PCM subsystem.
*  
*  Specifically that means:
*  - Program the SA1111 to use I2S data and L3 control channels
*  - Wake up the UDA1341 chip 
*/
static int snd_jornada720_probe(struct sa1111_dev *devptr) {
	struct snd_card *card;
	struct snd_jornada720 *jornada720;
	struct jornada720_model *m = NULL;
	int idx, err;
	int dev = 0;

	// Let the fun begin. Find the sa1111 chip
	if (machine_is_jornada720()) {
		// Call the SA1111 Audio init function
		sa1111_audio_init(devptr);
		
		// Program UDA1344 driver defaults
		err = uda1344_open(devptr);
		if (err < 0) {
			return err;
			printk(KERN_ERR "Jornada 720 soundcard could not initialize UDA1344 Codec\n");
		}

		// Turn on...
		err = sa1111_enable_device(devptr);
		if (err<0) {
			printk(KERN_ERR "Jornada 720 soundcard enable device failed.\n");
			return err;
		}

		// Test hardware setup (play startup sound)
		sa1111_audio_test(devptr);

		// Test DMA
		sa1111_test_dma(devptr);

		// Turn off...
		sa1111_disable_device(devptr);
	} else {
		printk(KERN_ERR "Jornada 720 soundcard not supported on this hardware\n");
		return -ENODEV;
	}

	// Register sound card with ALSA subsystem
	err = snd_card_new(&devptr->dev, 0, id, THIS_MODULE, sizeof(struct snd_jornada720), &card);
	if (err < 0) return err;

	jornada720 = card->private_data;
	jornada720->card = card;
	// save the pointers to our HW instances for use in the pcm routines
	jornada720->pchip_uda1344 = &uda_chip;
	jornada720->pdev_sa1111 = devptr;

	printk(KERN_INFO "snd-jornada720: Using model '%s' for card %i\n", model_uda1344.name, card->number);
	m = jornada720->model = &model_uda1344;

	if (pcm_substreams < 1)	pcm_substreams = 1;
	if (pcm_substreams > MAX_PCM_SUBSTREAMS) pcm_substreams = MAX_PCM_SUBSTREAMS;
	err = snd_card_jornada720_pcm(jornada720, idx, pcm_substreams);
	if (err < 0)
		goto __nodev;

	jornada720->pcm_hw = jornada720_pcm_hardware;
	if (m) {
		if (m->formats)
			jornada720->pcm_hw.formats = m->formats;
		if (m->buffer_bytes_max)
			jornada720->pcm_hw.buffer_bytes_max = m->buffer_bytes_max;
		if (m->period_bytes_min)
			jornada720->pcm_hw.period_bytes_min = m->period_bytes_min;
		if (m->period_bytes_max)
			jornada720->pcm_hw.period_bytes_max = m->period_bytes_max;
		if (m->periods_min)
			jornada720->pcm_hw.periods_min = m->periods_min;
		if (m->periods_max)
			jornada720->pcm_hw.periods_max = m->periods_max;
		if (m->rates)
			jornada720->pcm_hw.rates = m->rates;
		if (m->rate_min)
			jornada720->pcm_hw.rate_min = m->rate_min;
		if (m->rate_max)
			jornada720->pcm_hw.rate_max = m->rate_max;
		if (m->channels_min)
			jornada720->pcm_hw.channels_min = m->channels_min;
		if (m->channels_max)
			jornada720->pcm_hw.channels_max = m->channels_max;
	}

	err = snd_card_jornada720_new_mixer(jornada720);
	if (err < 0) goto __nodev;

	strcpy(card->driver, "Jornada 720");
	strcpy(card->shortname, "Jornada 720");
	sprintf(card->longname, "Jornada 720 %i", dev + 1);

	jornada720_proc_init(jornada720);

	err = snd_card_register(card);
	if (err == 0) {
		sa1111_set_drvdata(devptr, card);
		return 0;
	}
      __nodev:
	snd_card_free(card);
	return err;
}

static int snd_jornada720_remove(struct sa1111_dev *devptr) {
	uda1344_close(devptr);
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


static void snd_jornada720_unregister_all(void)
{
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

	/* Maybe allocate the DMA buffers here?
	err = alloc_fake_buffer();
	if (err < 0) {
		sa1111_driver_unregister(&snd_jornada720_driver);
		return err;
	}
	*/
	return 0;
}

static void __exit alsa_card_jornada720_exit(void)
{
	snd_jornada720_unregister_all();
}

module_init(alsa_card_jornada720_init)
module_exit(alsa_card_jornada720_exit)
