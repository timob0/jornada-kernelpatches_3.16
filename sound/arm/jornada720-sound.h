/*
 *  jornada720-sound.h
 *
 *  Toplevel include file for jornada720 sound driver.
 *
 *  Copyright (C) 2021 Timo Biesenbach
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */
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

#endif