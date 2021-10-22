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
#include <mach/hardware.h>
#include <asm/hardware/sa1111.h>
#include <asm/mach-types.h>
#include <mach/jornada720.h>
#include <asm/irq.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <sound/info.h>
#include <sound/initval.h>
#include <sound/uda134x.h>

// Testsound for debugging the codec remove later!
#include "octane.h"
// #include "cymbal.c"
// #include "fanfare.c"

MODULE_AUTHOR("Timo Biesenbach <timo.biesenbach@gmail.com>");
MODULE_DESCRIPTION("Jornada 720 Sound Driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{ALSA,Jornada 720 Sound Driver}}");

#define MAX_PCM_DEVICES		1
#define MAX_PCM_SUBSTREAMS	1
#define MAX_MIDI_DEVICES	0

/* defaults */
#define MAX_BUFFER_SIZE		(64*1024)
#define MIN_PERIOD_SIZE		64
#define MAX_PERIOD_SIZE		MAX_BUFFER_SIZE
#define USE_FORMATS 		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE)
#define USE_RATE		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000
#define USE_RATE_MIN		8000
#define USE_RATE_MAX		48000
#define USE_CHANNELS_MIN 	1
#define USE_CHANNELS_MAX 	2
#define USE_PERIODS_MIN 	1
#define USE_PERIODS_MAX 	1024

#define MIXER_ADDR_MASTER	0
#define MIXER_ADDR_MIC		2
#define MIXER_ADDR_LAST		4


// Lock 
static DEFINE_SPINLOCK(snd_jornada720_sa1111_lock);

// SA1111 L3 interface
static inline void         sa1111_sac_writereg(struct sa1111_dev *devptr, unsigned int val, u32 reg) {
	sa1111_writel(val, devptr->mapbase + reg);
}

static inline unsigned int sa1111_sac_readreg(struct sa1111_dev *devptr, u32 reg) {
	return sa1111_readl(devptr->mapbase + reg);
}

// L3 stuff
static inline void l3_sa1111_send_byte(struct sa1111_dev *devptr, unsigned char addr, unsigned char dat) {
	int i=0;
	unsigned int SASCR;
	unsigned int SACR1;
	
	// Make sure only one thread is in the critical section below.
	spin_lock(&snd_jornada720_sa1111_lock);
	
	sa1111_sac_writereg(devptr, 0, SA1111_L3_CAR);
	sa1111_sac_writereg(devptr, 0, SA1111_L3_CDR);
	mdelay(1);

	SASCR = sa1111_sac_readreg(devptr, SA1111_SASCR);
	SASCR = SASCR_DTS|SASCR_RDD;
	sa1111_sac_writereg(devptr, SASCR, SA1111_SASCR);

	sa1111_sac_writereg(devptr, addr,  SA1111_L3_CAR);
	sa1111_sac_writereg(devptr, dat,   SA1111_L3_CDR);

	while (((sa1111_sac_readreg(devptr, SA1111_SASR0) & SASR0_L3WD) == 0) && (i < 1000)) {
		mdelay(1);
		i++;
	}
	if (((sa1111_sac_readreg(devptr, SA1111_SASR0) & SASR0_L3WD) == 0)) {
		printk("Avoided crash in l3_sa1111_send_byte. Trying to reset L3.\n"); // <-- need to check this. doesnt sound right.
		SACR1 = sa1111_sac_readreg(devptr, SA1111_SACR1);
		SACR1 &= ~SACR1_L3EN;
		sa1111_sac_writereg(devptr, SACR1, SA1111_SACR1);

		mdelay(100);

		SACR1 = sa1111_sac_readreg(devptr, SA1111_SACR1);
		SACR1 |= SACR1_L3EN;
		sa1111_sac_writereg(devptr, SACR1, SA1111_SACR1);
	}
	
	SASCR = SASCR_DTS|SASCR_RDD;
	sa1111_sac_writereg(devptr, SASCR, SA1111_SASCR);
	
	// Give up the lock
	spin_unlock(&snd_jornada720_sa1111_lock);
}

// UDA134x stuff
#define UDA1341_NAME "uda1341"

#define DEF_VOLUME	65

/*
 * UDA134x L3 address and command types
 */
#define UDA1341_L3ADDR		5
#define UDA1341_DATA		(UDA1341_L3ADDR << 2 | 0)
#define UDA1341_STATUS		(UDA1341_L3ADDR << 2 | 2)

struct uda1341_regs {
	unsigned char	stat0;
#define STAT0			0x00
#define STAT0_SC_MASK		(3 << 4)  // System clock
#define STAT0_SC_512FS		(0 << 4)  // Systemclock 512/s
#define STAT0_SC_384FS		(1 << 4)  // Systemclock 384f/s
#define STAT0_SC_256FS		(2 << 4)  // Systemclock 256f/s
#define STAT0_SC_UNUSED		(3 << 4)  // Systemclock unused
#define STAT0_IF_MASK		(7 << 1)
#define STAT0_IF_I2S		(0 << 1)  // Data format I2S
#define STAT0_IF_LSB16		(1 << 1)  // Data LSB justified 16bit
#define STAT0_IF_LSB18		(2 << 1)  // Data LSB justified 18bit
#define STAT0_IF_LSB20		(3 << 1)  // Data LSB justified 20bit
#define STAT0_IF_MSB		(4 << 1)  // Data MSB justified
#define STAT0_IF_LSB16MSB	(5 << 1)  // Data MSB justified 16bit
#define STAT0_IF_LSB18MSB	(6 << 1)  // Data MSB justified 18bit
#define STAT0_IF_LSB20MSB	(7 << 1)  // Data MSB justified 20bit
#define STAT0_DC_FILTER		(1 << 0)  // Enable DC filter
	unsigned char	data0_0;
#define DATA0			0x00
#define DATA0_VOLUME_MASK	0x3f
#define DATA0_VOLUME(x)		(x)
	unsigned char	data0_1;
#define DATA1			0x40
#define DATA1_BASS(x)		((x) << 2)
#define DATA1_BASS_MASK		(15 << 2)
#define DATA1_TREBLE(x)		((x))
#define DATA1_TREBLE_MASK	(3)
	unsigned char	data0_2;
#define DATA2			0x80
#define DATA2_DEEMP_NONE	(0 << 3)
#define DATA2_DEEMP_32KHz	(1 << 3)
#define DATA2_DEEMP_44KHz	(2 << 3)
#define DATA2_DEEMP_48KHz	(3 << 3)
#define DATA2_MUTE			(1 << 2)
#define DATA2_FILTER_FLAT	(0 << 0)
#define DATA2_FILTER_MIN	(1 << 0)
#define DATA2_FILTER_MAX	(3 << 0)
	unsigned char	data0_3;
#define DATA3			0xc0
#define DATA3_POWER_OFF		(0 << 0)
#define DATA3_POWER_DAC		(1 << 0)
#define DATA3_POWER_ADC		(2 << 0)
#define DATA3_POWER_ON		(3 << 0)
};


struct uda1341 {
	struct uda1341_regs regs;
	int		active;
	unsigned short	volume;
	unsigned short	bass;
	unsigned short	treble;
	unsigned short	line;
	unsigned short	mic;
	int		mod_cnt;
};

// The UDA1341 chip instance
static struct uda1341 uda_chip = {
	.volume = DEF_VOLUME | DEF_VOLUME << 8,
	.bass   = 50 | 50 << 8,
	.treble = 50 | 50 << 8,
	.line   = 88 | 88 << 8,
	.mic    = 88 | 88 << 8,
	.regs.stat0   = STAT0_SC_512FS | STAT0_IF_LSB16, // <- set i2s interface and 256f/s
	.regs.data0_0 = DATA0_VOLUME(62 - ((DEF_VOLUME * 61) / 100)),
	.regs.data0_1 = DATA1_BASS(0) | DATA1_TREBLE(0),
	.regs.data0_2 = DATA2_DEEMP_NONE | DATA2_FILTER_FLAT,
	.regs.data0_3 = 0x00, // <-- Data 3 defintion is incomplete. We might not need it 
};

static void uda1341_sync(struct sa1111_dev *devptr) {
	struct uda1341 *uda = &uda_chip;
	l3_sa1111_send_byte(devptr, UDA1341_STATUS, STAT0 | uda->regs.stat0);
	l3_sa1111_send_byte(devptr, UDA1341_DATA,  DATA0 | uda->regs.data0_0);
	l3_sa1111_send_byte(devptr, UDA1341_DATA,  DATA1 | uda->regs.data0_1);
	l3_sa1111_send_byte(devptr, UDA1341_DATA,  DATA2 | uda->regs.data0_2);
	// l3_sa1111_send_byte(devptr, UDA1341_DATA,DATA3 | uda->regs.data0_3); This is wrong. Poweron default should be ok.
}

static void uda1341_cmd_init(struct sa1111_dev *devptr) {
	struct uda1341 *uda = &uda_chip;
	uda->active = 1;
	// Synchronize the configuration from uda_chip
	uda1341_sync(devptr);
}
/*
static int uda1341_update_direct(struct sa1111_dev *devptr, int cmd, void *arg) {
	struct uda1341 *uda = &uda_chip;
	struct l3_gain *v = arg;
	char newreg;
	int val;

	switch (cmd) {
	case L3_SET_VOLUME: / * set volume.  val =  0 to 100 => 62 to 1 * /
		uda->regs.data0_0 = DATA0_VOLUME(62 - ((v->left * 61) / 100));
		newreg = uda->regs.data0_0 | DATA0;
		break;

	case L3_SET_BASS:   / * set bass.    val = 50 to 100 => 0 to 12 * /
		val = v->left - 50;
		if (val < 0)
			val = 0;
		uda->regs.data0_1 &= ~DATA1_BASS_MASK;
		uda->regs.data0_1 |= DATA1_BASS((val * 12) / 50);
		newreg = uda->regs.data0_1 | DATA1;
		break;

	case L3_SET_TREBLE: / * set treble.  val = 50 to 100 => 0 to 3 * /
		val = v->left - 50;
		if (val < 0)
			val = 0;
		uda->regs.data0_1 &= ~DATA1_TREBLE_MASK;
		uda->regs.data0_1 |= DATA1_TREBLE((val * 3) / 50);
		newreg = uda->regs.data0_1 | DATA1;
		break;

	default:
		return -EINVAL;
	}		

	if (uda->active)
		l3_sa1111_send_byte(devptr, UDA1341_DATA, newreg);
	return 0;
}
*/

/**
 * Initialize the 1344 with some sensible defaults and turn on power.
 */
static void uda1344_reset(struct sa1111_dev *devptr) {
	unsigned char val; 

	val = STAT0 | STAT0_IF_I2S | STAT0_SC_512FS;
	l3_sa1111_send_byte(devptr, UDA1341_STATUS, val);
	printk(KERN_INFO "j720 uda1341 STAT0 programmed with: 0x%lxh\n", val);

	val = DATA0 | DATA0_VOLUME(0);  // <-- 0db volume : max
	l3_sa1111_send_byte(devptr, UDA1341_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA0 programmed with: 0x%lxh\n", val);

	val = DATA1 | DATA1_BASS(0) | DATA1_TREBLE(0);  // <-- 0db volume : max
	l3_sa1111_send_byte(devptr, UDA1341_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA1 programmed with: 0x%lxh\n", val);

	val = DATA2 | DATA2_DEEMP_NONE | DATA2_FILTER_FLAT;
	l3_sa1111_send_byte(devptr, UDA1341_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA3 programmed with: 0x%lxh\n", val);

	val = DATA3 | DATA3_POWER_ON;
	l3_sa1111_send_byte(devptr, UDA1341_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA3 programmed with: 0x%lxh\n", val);
}

/*
#define REC_MASK	(SOUND_MASK_LINE | SOUND_MASK_MIC)
#define DEV_MASK	(REC_MASK | SOUND_MASK_VOLUME | SOUND_MASK_BASS | SOUND_MASK_TREBLE)

static int uda1341_mixer_ioctl(struct l3_client *clnt, int cmd, void *arg)
{
	struct uda1341 *uda = clnt->driver_data;
	struct l3_gain gain;
	int val, nr = _IOC_NR(cmd), ret = 0;

	if (cmd == SOUND_MIXER_INFO) {
		struct mixer_info mi;

		strncpy(mi.id, "UDA1341", sizeof(mi.id));
		strncpy(mi.name, "Philips UDA1341", sizeof(mi.name));
		mi.modify_counter = uda->mod_cnt;
		return copy_to_user(arg, &mi, sizeof(mi));
	}

	if (_IOC_DIR(cmd) & _IOC_WRITE) {
		ret = get_user(val, (int *)arg);
		if (ret)
			goto out;

		gain.left    = val & 255;
		gain.right   = val >> 8;
		gain.channel = 0;

		switch (nr) {
		case SOUND_MIXER_VOLUME:
			uda->volume = val;
			uda->mod_cnt++;
			uda1341_update_direct(clnt, L3_SET_VOLUME, &gain);
			break;

		case SOUND_MIXER_BASS:
			uda->bass = val;
			uda->mod_cnt++;
			uda1341_update_direct(clnt, L3_SET_BASS, &gain);
			break;

		case SOUND_MIXER_TREBLE:
			uda->treble = val;
			uda->mod_cnt++;
			uda1341_update_direct(clnt, L3_SET_TREBLE, &gain);
			break;

		case SOUND_MIXER_LINE:
			ret = -EINVAL;
			break;

		case SOUND_MIXER_MIC:
			ret = -EINVAL;
			break;

		case SOUND_MIXER_RECSRC:
			break;

		default:
			ret = -EINVAL;
		}
	}

	if (ret == 0 && _IOC_DIR(cmd) & _IOC_READ) {
		int nr = _IOC_NR(cmd);
		ret = 0;

		switch (nr) {
		case SOUND_MIXER_VOLUME:     val = uda->volume;	break;
		case SOUND_MIXER_BASS:       val = uda->bass;	break;
		case SOUND_MIXER_TREBLE:     val = uda->treble;	break;
		case SOUND_MIXER_LINE:       val = uda->line;	break;
		case SOUND_MIXER_MIC:        val = uda->mic;	break;
		case SOUND_MIXER_RECSRC:     val = REC_MASK;	break;
		case SOUND_MIXER_RECMASK:    val = REC_MASK;	break;
		case SOUND_MIXER_DEVMASK:    val = DEV_MASK;	break;
		case SOUND_MIXER_CAPS:       val = 0;		break;
		case SOUND_MIXER_STEREODEVS: val = 0;		break;
		default:	val = 0;     ret = -EINVAL;	break;
		}

		if (ret == 0)
			ret = put_user(val, (int *)arg);
	}
out:
	return ret;
}
*/

static int uda1341_open(struct sa1111_dev *devptr) {
	uda1341_cmd_init(devptr);
	return 0;
}

static void uda1341_close(struct sa1111_dev *devptr) {
	struct uda1341 *uda = &uda_chip;
	uda->active = 0;
}

static void uda1341_set_samplerate(struct sa1111_dev *devptr, long rate) {
	struct uda1341 *uda = &uda_chip;
	int clk_div = 0;
	int clk=0;

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
	uda1341_sync(devptr);
}

// Module specific stuff

static char *id  = SNDRV_DEFAULT_STR1;
static bool enable = SNDRV_DEFAULT_ENABLE1;
static char *model = UDA1341_NAME;
static int pcm_devs = 1;
static int pcm_substreams = 1;
static bool fake_buffer = 1;

module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for Jornada 720 UDA1341 soundcard.");

static struct platform_device *device;

struct jornada720_timer_ops {
	int (*create)(struct snd_pcm_substream *);
	void (*free)(struct snd_pcm_substream *);
	int (*prepare)(struct snd_pcm_substream *);
	int (*start)(struct snd_pcm_substream *);
	int (*stop)(struct snd_pcm_substream *);
	snd_pcm_uframes_t (*pointer)(struct snd_pcm_substream *);
};

#define get_jornada720_ops(substream) \
	(*(const struct jornada720_timer_ops **)(substream)->runtime->private_data)

struct jornada720_model {
	const char *name;
	int (*playback_constraints)(struct snd_pcm_runtime *runtime);
	int (*capture_constraints)(struct snd_pcm_runtime *runtime);
	u64 formats;
	size_t buffer_bytes_max;
	size_t period_bytes_min;
	size_t period_bytes_max;
	unsigned int periods_min;
	unsigned int periods_max;
	unsigned int rates;
	unsigned int rate_min;
	unsigned int rate_max;
	unsigned int channels_min;
	unsigned int channels_max;
};

struct snd_jornada720 {
	struct snd_card *card;
	struct jornada720_model *model;
	struct snd_pcm *pcm;
	struct snd_pcm_hardware pcm_hw;
	spinlock_t mixer_lock;
	int mixer_volume[MIXER_ADDR_LAST+1][2];
	int capture_source[MIXER_ADDR_LAST+1][2];
	int iobox;
	struct snd_kcontrol *cd_volume_ctl;
	struct snd_kcontrol *cd_switch_ctl;
};

/*
 * card models
 */

struct jornada720_model model_uda1341 = {
	.name = "uda1341",
	.buffer_bytes_max = 16380,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.channels_min = 2,
	.channels_max = 2,
	.periods_min = 2,
	.periods_max = 255,
};

/*
 * system timer interface
 */

struct jornada720_systimer_pcm {
	/* ops must be the first item */
	const struct jornada720_timer_ops *timer_ops;
	spinlock_t lock;
	struct timer_list timer;
	unsigned long base_time;
	unsigned int frac_pos;	/* fractional sample position (based HZ) */
	unsigned int frac_period_rest;
	unsigned int frac_buffer_size;	/* buffer_size * HZ */
	unsigned int frac_period_size;	/* period_size * HZ */
	unsigned int rate;
	int elapsed;
	struct snd_pcm_substream *substream;
};

static void jornada720_systimer_rearm(struct jornada720_systimer_pcm *dpcm)
{
	dpcm->timer.expires = jiffies +
		(dpcm->frac_period_rest + dpcm->rate - 1) / dpcm->rate;
	add_timer(&dpcm->timer);
}

static void jornada720_systimer_update(struct jornada720_systimer_pcm *dpcm)
{
	unsigned long delta;

	delta = jiffies - dpcm->base_time;
	if (!delta)
		return;
	dpcm->base_time += delta;
	delta *= dpcm->rate;
	dpcm->frac_pos += delta;
	while (dpcm->frac_pos >= dpcm->frac_buffer_size)
		dpcm->frac_pos -= dpcm->frac_buffer_size;
	while (dpcm->frac_period_rest <= delta) {
		dpcm->elapsed++;
		dpcm->frac_period_rest += dpcm->frac_period_size;
	}
	dpcm->frac_period_rest -= delta;
}

static int jornada720_systimer_start(struct snd_pcm_substream *substream)
{
	struct jornada720_systimer_pcm *dpcm = substream->runtime->private_data;
	spin_lock(&dpcm->lock);
	dpcm->base_time = jiffies;
	jornada720_systimer_rearm(dpcm);
	spin_unlock(&dpcm->lock);
	return 0;
}

static int jornada720_systimer_stop(struct snd_pcm_substream *substream)
{
	struct jornada720_systimer_pcm *dpcm = substream->runtime->private_data;
	spin_lock(&dpcm->lock);
	del_timer(&dpcm->timer);
	spin_unlock(&dpcm->lock);
	return 0;
}

static int jornada720_systimer_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jornada720_systimer_pcm *dpcm = runtime->private_data;

	dpcm->frac_pos = 0;
	dpcm->rate = runtime->rate;
	dpcm->frac_buffer_size = runtime->buffer_size * HZ;
	dpcm->frac_period_size = runtime->period_size * HZ;
	dpcm->frac_period_rest = dpcm->frac_period_size;
	dpcm->elapsed = 0;

	return 0;
}

static void jornada720_systimer_callback(unsigned long data)
{
	struct jornada720_systimer_pcm *dpcm = (struct jornada720_systimer_pcm *)data;
	unsigned long flags;
	int elapsed = 0;
	
	spin_lock_irqsave(&dpcm->lock, flags);
	jornada720_systimer_update(dpcm);
	jornada720_systimer_rearm(dpcm);
	elapsed = dpcm->elapsed;
	dpcm->elapsed = 0;
	spin_unlock_irqrestore(&dpcm->lock, flags);
	if (elapsed)
		snd_pcm_period_elapsed(dpcm->substream);
}

static snd_pcm_uframes_t
jornada720_systimer_pointer(struct snd_pcm_substream *substream)
{
	struct jornada720_systimer_pcm *dpcm = substream->runtime->private_data;
	snd_pcm_uframes_t pos;

	spin_lock(&dpcm->lock);
	jornada720_systimer_update(dpcm);
	pos = dpcm->frac_pos / HZ;
	spin_unlock(&dpcm->lock);
	return pos;
}

static int jornada720_systimer_create(struct snd_pcm_substream *substream)
{
	struct jornada720_systimer_pcm *dpcm;

	dpcm = kzalloc(sizeof(*dpcm), GFP_KERNEL);
	if (!dpcm)
		return -ENOMEM;
	substream->runtime->private_data = dpcm;
	init_timer(&dpcm->timer);
	dpcm->timer.data = (unsigned long) dpcm;
	dpcm->timer.function = jornada720_systimer_callback;
	spin_lock_init(&dpcm->lock);
	dpcm->substream = substream;
	return 0;
}

static void jornada720_systimer_free(struct snd_pcm_substream *substream)
{
	kfree(substream->runtime->private_data);
}

static struct jornada720_timer_ops jornada720_systimer_ops = {
	.create =	jornada720_systimer_create,
	.free =		jornada720_systimer_free,
	.prepare =	jornada720_systimer_prepare,
	.start =	jornada720_systimer_start,
	.stop =		jornada720_systimer_stop,
	.pointer =	jornada720_systimer_pointer,
};

/*
 * PCM interface
 */

static int jornada720_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		return get_jornada720_ops(substream)->start(substream);
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return get_jornada720_ops(substream)->stop(substream);
	}
	return -EINVAL;
}

static int jornada720_pcm_prepare(struct snd_pcm_substream *substream)
{
	return get_jornada720_ops(substream)->prepare(substream);
}

static snd_pcm_uframes_t jornada720_pcm_pointer(struct snd_pcm_substream *substream)
{
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

static int jornada720_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
	if (fake_buffer) {
		/* runtime->dma_bytes has to be set manually to allow mmap */
		substream->runtime->dma_bytes = params_buffer_bytes(hw_params);
		return 0;
	}
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int jornada720_pcm_hw_free(struct snd_pcm_substream *substream)
{
	if (fake_buffer) return 0;
	return snd_pcm_lib_free_pages(substream);
}

static int jornada720_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_jornada720 *jornada720 = snd_pcm_substream_chip(substream);
	struct jornada720_model *model = jornada720->model;
	struct snd_pcm_runtime *runtime = substream->runtime;
	const struct jornada720_timer_ops *ops;
	int err;

	ops = &jornada720_systimer_ops;

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

static int jornada720_pcm_close(struct snd_pcm_substream *substream)
{
	get_jornada720_ops(substream)->free(substream);
	return 0;
}

/*
 * jornada720 buffer handling
 */

static void *jornada720_page[2];

static void free_fake_buffer(void)
{
	if (fake_buffer) {
		int i;
		for (i = 0; i < 2; i++)
			if (jornada720_page[i]) {
				free_page((unsigned long)jornada720_page[i]);
				jornada720_page[i] = NULL;
			}
	}
}

static int alloc_fake_buffer(void)
{
	int i;

	if (!fake_buffer)
		return 0;
	for (i = 0; i < 2; i++) {
		jornada720_page[i] = (void *)get_zeroed_page(GFP_KERNEL);
		if (!jornada720_page[i]) {
			free_fake_buffer();
			return -ENOMEM;
		}
	}
	return 0;
}

static int jornada720_pcm_copy(struct snd_pcm_substream *substream,
			  int channel, snd_pcm_uframes_t pos,
			  void __user *dst, snd_pcm_uframes_t count)
{
	return 0; /* do nothing */
}

static int jornada720_pcm_silence(struct snd_pcm_substream *substream,
			     int channel, snd_pcm_uframes_t pos,
			     snd_pcm_uframes_t count)
{
	return 0; /* do nothing */
}

static struct page *jornada720_pcm_page(struct snd_pcm_substream *substream,
				   unsigned long offset)
{
	return virt_to_page(jornada720_page[substream->stream]); /* the same page */
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

static struct snd_pcm_ops jornada720_pcm_ops_no_buf = {
	.open =		jornada720_pcm_open,
	.close =	jornada720_pcm_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	jornada720_pcm_hw_params,
	.hw_free =	jornada720_pcm_hw_free,
	.prepare =	jornada720_pcm_prepare,
	.trigger =	jornada720_pcm_trigger,
	.pointer =	jornada720_pcm_pointer,
	.copy =		jornada720_pcm_copy,
	.silence =	jornada720_pcm_silence,
	.page =		jornada720_pcm_page,
};

static int snd_card_jornada720_pcm(struct snd_jornada720 *jornada720, int device, int substreams)
{
	struct snd_pcm *pcm;
	struct snd_pcm_ops *ops;
	int err;

	err = snd_pcm_new(jornada720->card, "Jornada720 PCM", device, substreams, substreams, &pcm);
	if (err < 0) return err;

	jornada720->pcm = pcm;
	if (fake_buffer) ops = &jornada720_pcm_ops_no_buf;
	else             ops = &jornada720_pcm_ops;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, ops);
	pcm->private_data = jornada720;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Jornada720 PCM");

	if (!fake_buffer) {
		snd_pcm_lib_preallocate_pages_for_all(pcm,
			SNDRV_DMA_TYPE_CONTINUOUS,
			snd_dma_continuous_data(GFP_KERNEL),
			0, 64*1024);
	}
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

static int snd_jornada720_iobox_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *info) {
	const char *const names[] = { "None", "CD Player" };
	return snd_ctl_enum_info(info, 1, 2, names);
}

static int snd_jornada720_iobox_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *value) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);

	value->value.enumerated.item[0] = jornada720->iobox;
	return 0;
}

static int snd_jornada720_iobox_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *value) {
	struct snd_jornada720 *jornada720 = snd_kcontrol_chip(kcontrol);
	int changed;

	if (value->value.enumerated.item[0] > 1)
		return -EINVAL;

	changed = value->value.enumerated.item[0] != jornada720->iobox;
	if (changed) {
		jornada720->iobox = value->value.enumerated.item[0];

		if (jornada720->iobox) {
			jornada720->cd_volume_ctl->vd[0].access &=
				~SNDRV_CTL_ELEM_ACCESS_INACTIVE;
			jornada720->cd_switch_ctl->vd[0].access &=
				~SNDRV_CTL_ELEM_ACCESS_INACTIVE;
		} else {
			jornada720->cd_volume_ctl->vd[0].access |=
				SNDRV_CTL_ELEM_ACCESS_INACTIVE;
			jornada720->cd_switch_ctl->vd[0].access |=
				SNDRV_CTL_ELEM_ACCESS_INACTIVE;
		}

		snd_ctl_notify(jornada720->card, SNDRV_CTL_EVENT_MASK_INFO,
			       &jornada720->cd_volume_ctl->id);
		snd_ctl_notify(jornada720->card, SNDRV_CTL_EVENT_MASK_INFO,
			       &jornada720->cd_switch_ctl->id);
	}

	return changed;
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
	jornada720->iobox = 1;

	for (idx = 0; idx < ARRAY_SIZE(snd_jornada720_controls); idx++) {
		kcontrol = snd_ctl_new1(&snd_jornada720_controls[idx], jornada720);
		err = snd_ctl_add(card, kcontrol);
		if (err < 0) return err;

		if (!strcmp(kcontrol->id.name, "CD Volume")) jornada720->cd_volume_ctl = kcontrol;
		else if (!strcmp(kcontrol->id.name, "CD Capture Switch")) jornada720->cd_switch_ctl = kcontrol;

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
sa1111_audio_test(struct sa1111_dev *devptr) {
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

		if (round==0) {
			// Readout status register and fifo levlel
			val = sa1111_sac_readreg(devptr, SA1111_SASR0);
			printk(KERN_INFO "j720 sa1111 SASR0: 0x%lxh\n", val);

			val = (val >> 8) & 0x0f;
			printk(KERN_INFO "j720 sa1111 Tx FIFO level: %d\n", val);

			printk(KERN_INFO "j720 sa1111 Tx left channel 8bit  data: %lx\n", octanestart_wav[i]);
			printk(KERN_INFO "j720 sa1111 Tx left channel 16bit data: %lx\n", left);
			printk(KERN_INFO "j720 sa1111 Tx sample data            : %lx\n", sample);
			round++;
		}

		// Wait until FIFO not full
		do {
			val = sa1111_sac_readreg(devptr, SA1111_SASR0);
		} while((val & SASR0_TNF)==0);
	
		if (round==1) {
			// Readout status register and fifo levlel
			val = sa1111_sac_readreg(devptr, SA1111_SASR0);
			printk(KERN_INFO "j720 sa1111 SASR0: 0x%lxh\n", val);

			val = (val >> 8) & 0x0f;
			printk(KERN_INFO "j720 sa1111 Tx FIFO level: %d\n", val);

			printk(KERN_INFO "j720 sa1111 Tx left channel 8bit  data: %lx\n", octanestart_wav[i]);
			printk(KERN_INFO "j720 sa1111 Tx left channel 16bit data: %lx\n", left);
			printk(KERN_INFO "j720 sa1111 Tx sample data            : %lx\n", sample);
			round++;
		}	
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

// Copied from "glue audio driver"
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

	// deselect AC Link
	sa1111_select_audio_mode(devptr, SA1111_AUDIO_I2S);

	/* Enable the I2S clock and L3 bus clock. This is a function in another SA1111 block
	 * which is why we need the sachip stuff (should probably be a function in sa1111.c/h)
	 */
	val = sa1111_readl(sachip->base + SA1111_SKPCR);
	val|= (SKPCR_I2SCLKEN | SKPCR_L3CLKEN);
	sa1111_writel(val, sachip->base + SA1111_SKPCR);

	/* Activate and reset the Serial Audio Controller */
	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val |= (SACR0_ENB | SACR0_RST);
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);
	mdelay(5);
	val = sa1111_sac_readreg(devptr, SA1111_SACR0);
	val &= ~SACR0_RST;
	sa1111_sac_writereg(devptr, val, SA1111_SACR0);
	
	/* For I2S, BIT_CLK is supplied internally. The "SA-1111
	 * Specification Update" mentions that the BCKD bit should
	 * be interpreted as "0 = output". Default clock divider
	 * is 22.05kHz.
	 *
	 * Select I2S, L3 bus. "Recording" and "Replaying"
	 * (receive and transmit) are enabled.
	 */
	sa1111_sac_writereg(devptr, SACR1_L3EN, SA1111_SACR1);

	// Set samplerate
	sa1111_set_audio_rate(devptr, 22050);
	int rate = sa1111_get_audio_rate(devptr);
	printk(KERN_INFO "j720 sa1111 audio samplerate: %d\n", rate);

	// Reset the CODEC to defaults
	// What if we don't mess with it? Should be initialized by wince	
	uda1344_reset(devptr);

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
	struct jornada720_model *m = NULL, **mdl;
	int idx, err;
	int dev = 0;

	// Let the fun begin. Find the sa1111 chip
	if (machine_is_jornada720()) {
		// Call the SA1111 Audio init function
		sa1111_audio_init(devptr);

		// Test hardware setup
		sa1111_audio_test(devptr);

		// Program UDA1344 driver defaults
		// err = uda1341_open(devptr); <<- THIS IS BROKEN! Leaving the WinCE initialization seems to be better ;-)
		if (1==0 && err < 0) {
			return err;
			printk(KERN_ERR "Jornada 720 soundcard could not initialize UDA1341\n");
		}
	} else {
		printk(KERN_ERR "Jornada 720 soundcard not supported on this hardware\n");
		return -ENODEV;
	}

	// Register sound card with ALSA subsystem
	err = snd_card_new(&devptr->dev, 0, id, THIS_MODULE, sizeof(struct snd_jornada720), &card);
	if (err < 0) return err;

	jornada720 = card->private_data;
	jornada720->card = card;

	printk(KERN_INFO "snd-jornada720: Using model '%s' for card %i\n", model_uda1341.name, card->number);
	m = jornada720->model = &model_uda1341;


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

// static int snd_jornada720_remove(struct platform_device *devptr)
static int snd_jornada720_remove(struct sa1111_dev *devptr)
{
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
	free_fake_buffer();
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

	err = alloc_fake_buffer();
	if (err < 0) {
		sa1111_driver_unregister(&snd_jornada720_driver);
		return err;
	}
	return 0;
}

static void __exit alsa_card_jornada720_exit(void)
{
	snd_jornada720_unregister_all();
}

module_init(alsa_card_jornada720_init)
module_exit(alsa_card_jornada720_exit)
