#ifndef JORNADA720_SND_H
#define JORNADA720_SND_H

#define MAX_PCM_DEVICES		1
#define MAX_PCM_SUBSTREAMS	1
#define MAX_MIDI_DEVICES	0

/* defaults */
#define MAX_BUFFER_SIZE		(64*1024)
#define MIN_PERIOD_SIZE		64
#define MAX_PERIOD_SIZE		MAX_BUFFER_SIZE
#define USE_FORMATS 		(SNDRV_PCM_FMTBIT_S16_LE)
#define USE_RATE			SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000
#define USE_RATE_MIN		8000
#define USE_RATE_MAX		48000
#define USE_CHANNELS_MIN 	1
#define USE_CHANNELS_MAX 	2
#define USE_PERIODS_MIN 	1
#define USE_PERIODS_MAX 	1024

#define MIXER_ADDR_MASTER	0
#define MIXER_ADDR_MIC		2
#define MIXER_ADDR_LAST		4

// UDA134x stuff
#define UDA1344_NAME "uda1344"
#define DEF_VOLUME	65

/*
 * UDA134x L3 address and command types
 */
#define UDA1344_L3ADDR		5
#define UDA1344_DATA		(UDA1344_L3ADDR << 2 | 0)
#define UDA1344_STATUS		(UDA1344_L3ADDR << 2 | 2)

struct uda1344_regs {
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

struct uda1344 {
	struct uda1344_regs regs;
	int		        active;
	unsigned short	volume;
	unsigned short	bass;
	unsigned short	treble;
	unsigned short	line;
	unsigned short	mic;
	int		        mod_cnt;
	long			samplerate;

};

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
	//HW pointers
	struct uda1344* pchip_uda1344;
	struct sa1111_dev * pdev_sa1111;
};

/*
 * System timer interface  
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


// Trashbin
/* Initialize the 1344 with some sensible defaults and turn on power.
static void uda1344_reset(struct sa1111_dev *devptr) {
	unsigned char val; 

	val = STAT0 | STAT0_IF_I2S | STAT0_SC_512FS;
	sa1111_l3_send_byte(devptr, UDA1344_STATUS, val);
	printk(KERN_INFO "j720 uda1341 STAT0 programmed with: 0x%lxh\n", val);

	val = DATA0 | DATA0_VOLUME(0);  // <-- 0db volume : max
	sa1111_l3_send_byte(devptr, UDA1344_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA0 programmed with: 0x%lxh\n", val);

	val = DATA1 | DATA1_BASS(0) | DATA1_TREBLE(0);  // <-- 0db volume : max
	sa1111_l3_send_byte(devptr, UDA1344_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA1 programmed with: 0x%lxh\n", val);

	val = DATA2 | DATA2_DEEMP_NONE | DATA2_FILTER_FLAT;
	sa1111_l3_send_byte(devptr, UDA1344_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA3 programmed with: 0x%lxh\n", val);

	val = DATA3 | DATA3_POWER_ON;
	sa1111_l3_send_byte(devptr, UDA1344_DATA, val);
	printk(KERN_INFO "j720 uda1341 DATA3 programmed with: 0x%lxh\n", val);
}
*/
/*
#define REC_MASK	(SOUND_MASK_LINE | SOUND_MASK_MIC)
#define DEV_MASK	(REC_MASK | SOUND_MASK_VOLUME | SOUND_MASK_BASS | SOUND_MASK_TREBLE)

static int UDA1344_mixer_ioctl(struct l3_client *clnt, int cmd, void *arg)
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
			UDA1344_update_direct(clnt, L3_SET_VOLUME, &gain);
			break;

		case SOUND_MIXER_BASS:
			uda->bass = val;
			uda->mod_cnt++;
			UDA1344_update_direct(clnt, L3_SET_BASS, &gain);
			break;

		case SOUND_MIXER_TREBLE:
			uda->treble = val;
			uda->mod_cnt++;
			UDA1344_update_direct(clnt, L3_SET_TREBLE, &gain);
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

#endif