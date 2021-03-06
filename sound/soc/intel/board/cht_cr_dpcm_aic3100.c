/*
 *  cht_cr_dpcm_aic3100.c - ASoc Machine driver for Intel Cherrytrail CR platform
 *
 *  Copyright (C) 2014 Intel Corp
 *  Author: Dharageswari R <dharageswari.r@intel.com>
 *  This file is modified from byt_cr_aic3100.c for cherrytrail
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/acpi_gpio.h>
#include <linux/input.h>
#include <asm/intel-mid.h>
#include <asm/platform_cht_audio.h>
#include <asm/intel_soc_pmc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include "../../codecs/tlv320aic31xx.h"

/*FIXME*/
#define CHT_PLAT_CLK_3_HZ	25000000

#define CHT_INTR_DEBOUNCE               0
#define CHT_HS_INSERT_DET_DELAY         100
#define CHT_HS_REMOVE_DET_DELAY         100
#define CHT_BUTTON_PRESS_DELAY          50
#define CHT_BUTTON_RELEASE_DELAY        50
#define CHT_HS_DET_POLL_INTRVL          100
#define CHT_BUTTON_EN_DELAY             1500

#define CHT_HS_DET_RETRY_COUNT          6

#define VLV2_PLAT_CLK_AUDIO	3
#define PLAT_CLK_FORCE_ON	1
#define PLAT_CLK_FORCE_OFF	2

/* 0 = 25MHz from crystal, 1 = 19.2MHz from PLL */
/*FIXME*/
#define PLAT_CLK_FREQ_XTAL	0


struct cht_mc_private {
	struct snd_soc_jack jack;
	struct delayed_work hs_insert_work;
	struct delayed_work hs_remove_work;
	struct delayed_work hs_button_press_work;
	struct delayed_work hs_button_release_work;
	struct mutex jack_mlock;
	/* To enable button press interrupts after a delay after
	   HS detection. This is to avoid spurious button press
	   events during slow HS insertion */
	struct delayed_work hs_button_en_work;
	int intr_debounce;
	int hs_insert_det_delay;
	int hs_remove_det_delay;
	int button_press_delay;
	int button_release_delay;
	int button_en_delay;
	int hs_det_poll_intrvl;
	int hs_det_retry;
	bool process_button_events;
};

struct cht_slot_info {
	unsigned int tx_mask;
	unsigned int rx_mask;
	int slots;
	int slot_width;
};

static const struct snd_soc_pcm_stream cht_dai_params_ssp0 = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = SNDRV_PCM_RATE_48000,
	.rate_max = SNDRV_PCM_RATE_48000,
	.channels_min = 2,
	.channels_max = 2,
};

static const struct snd_soc_pcm_stream cht_dai_params_ssp1_fm = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = SNDRV_PCM_RATE_48000,
	.rate_max = SNDRV_PCM_RATE_48000,
	.channels_min = 2,
	.channels_max = 2,
};

static const struct snd_soc_pcm_stream cht_dai_params_ssp1_bt_nb = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = SNDRV_PCM_RATE_8000,
	.rate_max = SNDRV_PCM_RATE_8000,
	.channels_min = 2,
	.channels_max = 2,
};

static const struct snd_soc_pcm_stream cht_dai_params_ssp1_bt_wb = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = SNDRV_PCM_RATE_16000,
	.rate_max = SNDRV_PCM_RATE_16000,
	.channels_min = 2,
	.channels_max = 2,
};

static const struct snd_soc_pcm_stream cht_dai_params_ssp2 = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = SNDRV_PCM_RATE_48000,
	.rate_max = SNDRV_PCM_RATE_48000,
	.channels_min = 2,
	.channels_max = 2,
};


#define SST_BT_FM_MUX_SHIFT	0

static int cht_hs_detection(void);
static struct snd_soc_jack_gpio hs_gpio = {
		.name			= "cht-codec-int",
		.report			= SND_JACK_HEADSET |
					  SND_JACK_HEADPHONE |
					  SND_JACK_BTN_0,
		.debounce_time		= CHT_INTR_DEBOUNCE,
		.jack_status_check	= cht_hs_detection,
};


static inline void cht_force_enable_pin(struct snd_soc_codec *codec,
			 const char *bias_widget, bool enable)
{
	pr_debug("%s %s\n", enable ? "enable" : "disable", bias_widget);
	if (enable)
		snd_soc_dapm_force_enable_pin(&codec->dapm, bias_widget);
	else
		snd_soc_dapm_disable_pin(&codec->dapm, bias_widget);
}

static inline void cht_set_mic_bias(struct snd_soc_codec *codec, bool enable)
{
	if (enable)
		cht_force_enable_pin(codec, "micbias", true);
	else
		cht_force_enable_pin(codec, "micbias", false);
	snd_soc_dapm_sync(&codec->dapm);
}
/* Identify the jack type as Headset/Headphone/None */
static int cht_check_jack_type(void)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	int status, jack_type = 0;
	struct cht_mc_private *ctx =
			 container_of(jack, struct cht_mc_private, jack);

	status = aic31xx_query_jack_status(codec);
	/* jd status high indicates some accessory has been connected */
	if (status) {
		pr_debug("Jack insert intr");
		/* Do not process button events until accessory is
		 * detected as headset
		 */
		ctx->process_button_events = false;
		cht_set_mic_bias(codec, true);
		jack_type = aic31xx_query_jack_status(codec);
		if (jack_type == SND_JACK_HEADSET) {
			ctx->process_button_events = true;
			/* If headset is detected, enable button
			 * interrupts after a delay
			 */
			schedule_delayed_work(&ctx->hs_button_en_work,
					msecs_to_jiffies(ctx->button_en_delay));
		}
		if (jack_type != SND_JACK_HEADSET)
			cht_set_mic_bias(codec, false);
	} else {
		jack_type = 0;
	}
	pr_debug("Jack type detected:%d", jack_type);
	return jack_type;
}

/* Work function invoked by the Jack Infrastructure. Other delayed works
 * for jack detection/removal/button press are scheduled from this function
 */
static int cht_hs_detection(void)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	int status, jack_type = 0;
	int ret, val;
	struct cht_mc_private *ctx =
		 container_of(jack, struct cht_mc_private, jack);

	/* Ack interrupt first */
	val = snd_soc_read(codec, AIC31XX_INTRDACFLAG);
	mutex_lock(&ctx->jack_mlock);
	/* Initialize jack status with previous status.
	 * The delayed work will confirm the event and
	 * send updated status later
	 */
	jack_type = jack->status;
	pr_debug("Enter:%s", __func__);

	if (!jack->status) {
		ctx->hs_det_retry = CHT_HS_DET_RETRY_COUNT;
		ret = schedule_delayed_work(&ctx->hs_insert_work,
				msecs_to_jiffies(ctx->hs_insert_det_delay));
		if (!ret)
			pr_debug("cht_check_hs_insert_status already queued");
		else
			pr_debug("%s:Check hs insertion  after %d msec",
					__func__, ctx->hs_insert_det_delay);
	}  else {
		/* First check for accessory removal; If not removed,
		 * check for button events
		 */
		status = aic31xx_query_jack_status(codec);
		/* jd status low indicates accessory has been disconnected.
		 * However, confirm the removal in the delayed work
		 */
		if (!status) {
			/* Do not process button events while we make sure
			 * accessory is disconnected
			 */
			ctx->process_button_events = false;
			ret = schedule_delayed_work(&ctx->hs_remove_work,
				msecs_to_jiffies(ctx->hs_remove_det_delay));
			if (!ret)
				pr_debug("cht_check_hs_remove_status already queued");
			else
				pr_debug("%s:Check hs removal after %d msec",
						__func__,
						ctx->hs_remove_det_delay);
		} else { /* Must be button event.
			  * Confirm the event in delayed work
			  */
			if (((jack->status & SND_JACK_HEADSET) == SND_JACK_HEADSET) &&
					ctx->process_button_events) {
				ret = schedule_delayed_work(
					&ctx->hs_button_press_work,
					msecs_to_jiffies(
						ctx->button_press_delay));
				if (!ret)
					pr_debug("cht_check_hs_button_press_status already queued");
				else
					pr_debug("%s:check BP/BR after %d msec",
						__func__,
						ctx->button_press_delay);
			}
		}
	}

	pr_debug("Exit:%s", __func__);
	mutex_unlock(&ctx->jack_mlock);

	return jack_type;
}

/*Checks jack insertion and identifies the jack type.
 *Retries the detection if necessary
 */
static void cht_check_hs_insert_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct cht_mc_private *ctx =
		 container_of(work, struct cht_mc_private, hs_insert_work.work);
	int jack_type = 0;

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s", __func__);

	jack_type = cht_check_jack_type();

	/* Report jack immediately only if jack is headset. If headphone or
	 * no jack was detected, dont report it until the last HS det try.
	 * This is to avoid reporting any temporary jack removal or accessory
	 * change(eg, HP to HS) during the detection tries. This provides
	 * additional debounce that will help in the case of slow insertion.
	 * This also avoids the pause in audio due to accessory
	 * change from HP to HS
	 */
	if (ctx->hs_det_retry <= 0) /* end of retries; report the status */{
		pr_debug("%d Jack type sent is %d\n", __LINE__, jack_type);
		snd_soc_jack_report(jack, jack_type, gpio->report);
	} else {
		/* Schedule another detection try if headphone or
		 * no jack is detected.
		 * During slow insertion of headset, first a headphone
		 * may be detected.
		 * Hence retry until headset is detected
		 */
		if (jack_type == SND_JACK_HEADSET) {
			ctx->hs_det_retry = 0;
			/* HS detected, no more retries needed */
			pr_debug("%d Jack type sent is %d\n",
				 __LINE__, jack_type);
			snd_soc_jack_report(jack, jack_type, gpio->report);
		} else {
			ctx->hs_det_retry--;
			schedule_delayed_work(&ctx->hs_insert_work,
				msecs_to_jiffies(ctx->hs_det_poll_intrvl));
			pr_debug("%s:re-try hs detection after %d msec",
					__func__, ctx->hs_det_poll_intrvl);
		}
	}

	pr_debug("Exit:%s", __func__);
	mutex_unlock(&ctx->jack_mlock);
}
/* Checks jack removal. */
static void cht_check_hs_remove_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	struct cht_mc_private *ctx =
		 container_of(work, struct cht_mc_private, hs_remove_work.work);
	int status = 0, jack_type = 0;

	/* Cancel any pending insertion detection. There
	   could be pending insertion detection in the
	   case of very slow insertion or insertion and
	   immediate removal.*/
	cancel_delayed_work_sync(&ctx->hs_insert_work);

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s", __func__);
	/* Initialize jack_type with previous status.
	   If the event was an invalid one, we return the preious state*/
	jack_type = jack->status;

	if (jack->status) {
		/* jack is in connected state; look for removal event */
		status = aic31xx_query_jack_status(codec);
		if (!status) {
			pr_debug("Jack remove event");
			ctx->process_button_events = false;
			cancel_delayed_work_sync(&ctx->hs_button_en_work);
			jack_type = 0;
			cht_set_mic_bias(codec, false);
			aic31xx_btn_press_intr_enable(codec, false);

		} else if (((jack->status & SND_JACK_HEADSET) == SND_JACK_HEADSET) &&
				 !ctx->process_button_events) {
			/* Jack is still connected. We may come here if
			   there was a spurious jack removal event.
			   No state change is done until removal is confirmed
			   by the check_jd_status above.i.e. jack status
			   remains Headset or headphone. But as soon as
			   the interrupt thread(cht_hs_detection) detected a jack
			   removal, button processing gets disabled.
			   Hence re-enable button processing in the case of
			   headset */
			pr_debug("spurious Jack remove event for headset re-enable button events");
			ctx->process_button_events = true;
		}
	}
	pr_debug("%d Jack type sent is %d\n", __LINE__, jack_type);
	snd_soc_jack_report(jack, jack_type, gpio->report);
	pr_debug("Exit:%s", __func__);
	mutex_unlock(&ctx->jack_mlock);
}
/* Check for button press status */
static void cht_check_hs_button_press_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	struct cht_mc_private *ctx =
		 container_of(work, struct cht_mc_private,
		 hs_button_press_work.work);
	int status = 0, jack_type = 0;
	int ret;

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s\n", __func__);
	jack_type = jack->status;

	if (((jack->status & SND_JACK_HEADSET) == SND_JACK_HEADSET)
			&& ctx->process_button_events) {

		status = aic31xx_query_jack_status(codec);
		if (status) { /* confirm jack is connected */

			status = aic31xx_query_btn_press(codec);
			if (status & SND_JACK_BTN_0) {
				jack_type = SND_JACK_HEADSET | SND_JACK_BTN_0;
				pr_debug("%d Jack type sent is %d\n",
					 __LINE__, jack_type);
				snd_soc_jack_report(jack,
						 jack_type, gpio->report);
				/* Since there is not button_relese interrupt
				   schedule delayed work to poll for button
				   release status
				 */
				ret = schedule_delayed_work(
					&ctx->hs_button_release_work,
					msecs_to_jiffies(
						ctx->button_release_delay));
			}
		}
	}
	pr_debug("Exit:%s\n", __func__);
	mutex_unlock(&ctx->jack_mlock);
}

/* Check for button release */
static void cht_check_hs_button_release_status(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	struct cht_mc_private *ctx = container_of(work, struct cht_mc_private,
			 hs_button_release_work.work);
	int status = 0, jack_type = 0;
	int ret;

	mutex_lock(&ctx->jack_mlock);
	pr_debug("Enter:%s\n", __func__);
	jack_type = jack->status;

	if (((jack->status & SND_JACK_HEADSET) == SND_JACK_HEADSET)
			&& ctx->process_button_events) {

		status = aic31xx_query_jack_status(codec);
		if (status) { /* confirm jack is connected */

			status = aic31xx_query_btn_press(codec);
			if (!(status & SND_JACK_BTN_0)) {
				jack_type = SND_JACK_HEADSET;
				pr_debug("%d Jack type sent is %d\n",
					__LINE__, jack_type);
				snd_soc_jack_report(jack, jack_type,
					 gpio->report);
			} else {
				/* Schedule again */
				ret = schedule_delayed_work(&ctx->hs_button_release_work,
					msecs_to_jiffies(
					ctx->button_release_delay));
			}

		}
	}

	pr_debug("Exit:%s\n", __func__);
	mutex_unlock(&ctx->jack_mlock);
}

/* Delayed work for enabling the overcurrent detection circuit and interrupt
   for generating button events */
static void cht_enable_hs_button_events(struct work_struct *work)
{
	struct snd_soc_jack_gpio *gpio = &hs_gpio;
	struct snd_soc_jack *jack = gpio->jack;
	struct snd_soc_codec *codec = jack->codec;
	struct cht_mc_private *ctx = container_of(work, struct cht_mc_private,
			 hs_button_en_work.work);

	aic31xx_btn_press_intr_enable(codec, ctx->process_button_events);
}

static inline struct snd_soc_codec *cht_get_codec(struct snd_soc_card *card)
{
	bool found = false;
	struct snd_soc_codec *codec;

	list_for_each_entry(codec, &card->codec_dev_list, card_list) {
		if (!strstr(codec->name, "tlv320aic31xx-codec")) {
			pr_debug("codec was %s", codec->name);
			continue;
		} else {
			found = true;
			break;
		}
	}
	if (found == false) {
		pr_err("%s: cant find codec", __func__);
		return NULL;
	}
	return codec;
}

static int platform_clock_control(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *k, int  event)
{

	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_codec *codec;
	int ret = 0;
	codec = cht_get_codec(card);
	if (!codec) {
		pr_err("Codec not found; Unable to set platform clock\n");
		return -EIO;
	}
	if (SND_SOC_DAPM_EVENT_ON(event)) {
		/*FIXME*/
		pmc_pc_configure(VLV2_PLAT_CLK_AUDIO,
				PLAT_CLK_FORCE_ON);
		pr_debug("Platform clk turned ON\n");
		snd_soc_codec_set_sysclk(codec, AIC31XX_MCLK,
				0, AIC31XX_FREQ_25000000, SND_SOC_CLOCK_IN);
		pr_debug("%d Jack_type detected = %d\n", __LINE__, ret);
	} else {
		/* Set codec clock source to internal clock before
		   turning off the platform clock. Codec needs clock
		   for Jack detection and button press */
		snd_soc_codec_set_sysclk(codec, AIC31XX_INTERNALCLOCK,
				0, 0, SND_SOC_CLOCK_IN);
		/*FIXME*/
		pmc_pc_configure(VLV2_PLAT_CLK_AUDIO,
				PLAT_CLK_FORCE_OFF);
		pr_debug("Platform clk turned OFF\n");
	}

	return 0;
}

/* machine DAPM */
static const struct snd_soc_dapm_widget cht_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
	SND_SOC_DAPM_SUPPLY("Platform Clock", SND_SOC_NOPM, 0, 0,
			platform_clock_control, SND_SOC_DAPM_PRE_PMU|
			SND_SOC_DAPM_POST_PMD),
};
static const struct snd_kcontrol_new cht_mc_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
	SOC_DAPM_PIN_SWITCH("Ext Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
};
static const struct snd_soc_dapm_route cht_audio_map[] = {
	/* External Speakers: HFL, HFR */
	{"Ext Spk", NULL, "SPK"},
	{"micbias", NULL, "Internal Mic"},
	/* Headset Mic: Headset Mic with bias */
	{"micbias", NULL, "Headset Mic"},

	/* Headset Stereophone(Headphone): HSOL, HSOR */
	{"Headphone", NULL, "HPL"},
	{"Headphone", NULL, "HPR"},

	{"Playback", NULL, "ssp2 Tx"},
	{"ssp2 Tx", NULL, "codec_out0"},
	{"ssp2 Tx", NULL, "codec_out1"},
	{"codec_in0", NULL, "ssp2 Rx"},
	{"codec_in1", NULL, "ssp2 Rx"},
	{"ssp2 Rx", NULL, "Capture"},
	{"ssp0 Tx", NULL, "modem_out"},
	{"modem_in", NULL, "ssp0 Rx"},
	{"ssp1 Tx", NULL, "bt_fm_out"},
	{"bt_fm_in", NULL, "ssp1 Rx"},
	{"Playback", NULL, "Platform Clock"},
	{"Capture", NULL, "Platform Clock"},
};

/* Sets dai format and pll */
static int cht_set_dai_fmt_pll(struct snd_soc_dai *codec_dai,
					int source, unsigned int freq_out)
{
	int ret;
	unsigned int fmt;
	/* Set codec DAI configuration */
	/* I2S Slave Mode`*/
	fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;
	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret < 0) {
		pr_err("can't set codec DAI configuration %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_pll(codec_dai, 0, source,
			CHT_PLAT_CLK_3_HZ, freq_out);
	if (ret < 0) {
		pr_err("can't set codec pll: %d\n", ret);
		return ret;
	}
	return 0;
}


static int cht_aif1_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	pr_debug("Enter:%s", __func__);
	/* Setecodec DAI confinuration */
	if (strncmp(codec_dai->name, "tlv320aic31xx-codec", 19))
		return 0;

	return cht_set_dai_fmt_pll(codec_dai, AIC31XX_PLL_CLKIN_MCLK,
			params_rate(params));
}

static const struct snd_soc_pcm_stream cht_dai_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = SNDRV_PCM_RATE_48000,
	.rate_max = SNDRV_PCM_RATE_48000,
	.channels_min = 2,
	.channels_max = 2,
};

#define SST_MUX_REG 25
#define SST_BT_FM_MUX_SHIFT	0
#define SST_BT_MODE_SHIFT	2
#define CHT_CONFIG_SLOT(slot_tx_mask, slot_rx_mask, num_slot, width)\
	(struct cht_slot_info){ .tx_mask = slot_tx_mask,			\
				  .rx_mask = slot_rx_mask,			\
				  .slots = num_slot,				\
				  .slot_width = width, }

static int cht_set_slot_and_format(struct snd_soc_dai *dai,
			struct cht_slot_info *slot_info, unsigned int fmt)
{
	int ret;

	ret = snd_soc_dai_set_tdm_slot(dai, slot_info->tx_mask,
		slot_info->rx_mask, slot_info->slots, slot_info->slot_width);
	if (ret < 0) {
		pr_err("can't set codec pcm format %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(dai, fmt);
	if (ret < 0) {
		pr_err("can't set codec DAI configuration %d\n", ret);
		return ret;
	}
	return ret;
}

static int cht_codec_fixup(struct snd_soc_pcm_runtime *rtd,
			    struct snd_pcm_hw_params *params)
{
	int ret = 0;
	unsigned int fmt;
	struct cht_slot_info *info;
	struct snd_interval *rate =  hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);

	/* WM8958 slave Mode */
	fmt =   SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;

	info = &CHT_CONFIG_SLOT(0x3, 0x3, 2, SNDRV_PCM_FORMAT_S24_LE);

	ret = cht_set_slot_and_format(rtd->cpu_dai, info, fmt);

	pr_debug("Invoked %s for dailink %s\n", __func__, rtd->dai_link->name);

	rate->min = rate->max = SNDRV_PCM_RATE_48000;
	channels->min = channels->max = 2;

	/* set SSP2 to 24-bit */
	snd_mask_set(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT -
					SNDRV_PCM_HW_PARAM_FIRST_MASK],
					SNDRV_PCM_FORMAT_S24_LE);
	return 0;
}

static int cht_bt_fm_fixup(struct snd_soc_dai_link *dai_link, struct snd_soc_dai *dai)
{
	unsigned int fmt;
	bool is_bt, is_bt_wb;
	unsigned int mask, reg_val;
	int ret;
	struct cht_slot_info *info;

	mask = (1 << fls(1)) - 1;
	reg_val = snd_soc_platform_read(dai->platform, SST_MUX_REG);
	is_bt = (reg_val >> SST_BT_FM_MUX_SHIFT) & mask;
	is_bt_wb = (reg_val >> SST_BT_MODE_SHIFT) & mask;

	if (is_bt) {
		if (is_bt_wb)
			dai_link->params = &cht_dai_params_ssp1_bt_wb;
		else
			dai_link->params = &cht_dai_params_ssp1_bt_nb;

		fmt = SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_CBS_CFS;
		info = &CHT_CONFIG_SLOT(0x01, 0x01, 1, SNDRV_PCM_FORMAT_S16_LE);
	} else {
		fmt = SND_SOC_DAIFMT_IB_NF | SND_SOC_DAIFMT_LEFT_J | SND_SOC_DAIFMT_CBS_CFS;
		dai_link->params = &cht_dai_params_ssp1_fm;
		info = &CHT_CONFIG_SLOT(0x03, 0x03, 2, SNDRV_PCM_FORMAT_S16_LE);
	}
	ret = cht_set_slot_and_format(dai, info, fmt);

	return ret;
}

static int cht_modem_fixup(struct snd_soc_dai_link *dai_link, struct snd_soc_dai *dai)
{
	int ret;
	unsigned int fmt;
	struct cht_slot_info *info;

	info = &CHT_CONFIG_SLOT(0x01, 0x01, 1, SNDRV_PCM_FORMAT_S16_LE);
	fmt = SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_IB_NF
						| SND_SOC_DAIFMT_CBS_CFS;

	ret = cht_set_slot_and_format(dai, info, fmt);
	return 0;
}

static int cht_codec_loop_fixup(struct snd_soc_dai_link *dai_link, struct snd_soc_dai *dai)
{
	int ret;
	unsigned int fmt;
	struct cht_slot_info *info;

	/* WM8958 slave Mode */
	fmt =   SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;

	info = &CHT_CONFIG_SLOT(0x3, 0x3, 2, SNDRV_PCM_FORMAT_S24_LE);

	ret = cht_set_slot_and_format(dai, info, fmt);

	return ret;
}

static int cht_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level)
{
	switch (level) {
	case SND_SOC_BIAS_ON:
	case SND_SOC_BIAS_PREPARE:
	case SND_SOC_BIAS_STANDBY:
	case SND_SOC_BIAS_OFF:
		break;
	default:
		pr_err("%s: Invalid bias level=%d\n", __func__, level);
		return -EINVAL;
	}
	card->dapm.bias_level = level;
	pr_debug("card(%s)->bias_level %u\n", card->name,
			card->dapm.bias_level);
	return 0;
}

static int cht_init(struct snd_soc_pcm_runtime *runtime)
{
	int ret;
	struct snd_soc_codec *codec;
	struct snd_soc_card *card = runtime->card;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(runtime->card);
	pr_debug("Enter:%s", __func__);

	codec = cht_get_codec(card);
	if (!codec) {
		pr_err("Codec not found: %s:failed\n", __func__);
		return -EIO;
	}
	/* Set codec bias level */
	cht_set_bias_level(card, &card->dapm, SND_SOC_BIAS_OFF);
	card->dapm.idle_bias_off = true;

	/* Headset jack detection */
	ret = snd_soc_jack_new(codec, "Headset Jack",
			SND_JACK_HEADSET | SND_JACK_HEADPHONE | SND_JACK_BTN_0,
			 &ctx->jack);
	if (ret) {
		pr_err("Jack creation failed\n");
		return ret;
	}
	snd_jack_set_key(ctx->jack.jack, SND_JACK_BTN_0, KEY_MEDIA);

	ret = snd_soc_jack_add_gpios(&ctx->jack, 1, &hs_gpio);
	if (ret) {
		pr_err("Adding jack GPIO failed with error %d\n", ret);
		return ret;
	}
	ret = snd_soc_add_card_controls(card, cht_mc_controls,
					ARRAY_SIZE(cht_mc_controls));
	if (ret) {
		pr_err("unable to add card controls\n");
		return ret;
	}
	ret = snd_soc_dapm_sync(&card->dapm);
	if (ret) {
		pr_err("unable to sync dapm\n");
		return ret;
	}
	return ret;
}

static unsigned int rates_8000_16000[] = {
	8000,
	16000,
};

static struct snd_pcm_hw_constraint_list constraints_8000_16000 = {
	.count = ARRAY_SIZE(rates_8000_16000),
	.list = rates_8000_16000,
};

static unsigned int rates_48000[] = {
	48000,
};

static struct snd_pcm_hw_constraint_list constraints_48000 = {
	.count = ARRAY_SIZE(rates_48000),
	.list  = rates_48000,
};

static int cht_aif1_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE,
			&constraints_48000);
}

static struct snd_soc_ops cht_aif1_ops = {
	.startup = cht_aif1_startup,
};

static int cht_8k_16k_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
		SNDRV_PCM_HW_PARAM_RATE,
		&constraints_8000_16000);
}
static struct snd_soc_ops cht_8k_16k_ops = {
	.startup = cht_8k_16k_startup,
	.hw_params = cht_aif1_hw_params,
};

static struct snd_soc_ops cht_be_ssp2_ops = {
	.hw_params = cht_aif1_hw_params,
};

static struct snd_soc_dai_link cht_dailink[] = {
	[CHT_DPCM_AUD_AIF1] = {
		.name = "Cherrytrail Audio Port",
		.stream_name = "Cherrytrail Audio",
		.cpu_dai_name = "Headset-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-platform",
		.init = cht_init,
		.ignore_suspend = 1,
		.dynamic = 1,
		.ops = &cht_aif1_ops,
	},
	[CHT_DPCM_DB] = {
		.name = "Cherrytrail DB Audio Port",
		.stream_name = "Deep Buffer Audio",
		.cpu_dai_name = "Deepbuffer-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-platform",
		.ignore_suspend = 1,
		.dynamic = 1,
		.ops = &cht_aif1_ops,
	},
	[CHT_DPCM_LL] = {
		.name = "Cherrytrail LL Audio Port",
		.stream_name = "Low Latency Audio",
		.cpu_dai_name = "Lowlatency-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-platform",
		.ignore_suspend = 1,
		.dynamic = 1,
		.ops = &cht_aif1_ops,
	},
	[CHT_DPCM_COMPR] = {
		.name = "Cherrytrail Compressed Port",
		.stream_name = "Cherrytrail Compress",
		.cpu_dai_name = "Compress-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-platform",
	},
	[CHT_DPCM_VOIP] = {
		.name = "Cherrytrail VOIP Port",
		.stream_name = "Cherrytrail Voip",
		.cpu_dai_name = "Voip-cpu-dai",
		.platform_name = "sst-platform",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.init = NULL,
		.ignore_suspend = 1,
		.ops = &cht_8k_16k_ops,
		.dynamic = 1,
	},
	[CHT_DPCM_PROBE] = {
		.name = "Cherrytrail Probe Port",
		.stream_name = "Cherrytrail Probe",
		.cpu_dai_name = "Probe-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-platform",
		.playback_count = 8,
		.capture_count = 8,
	},

	/* CODEC<->CODEC link */
	{
		.name = "Cherrytrail Codec-Loop Port",
		.stream_name = "Cherrytrail Codec-Loop",
		.cpu_dai_name = "ssp2-port",
		.platform_name = "sst-platform",
		.codec_dai_name = "tlv320aic31xx-codec",
		.codec_name = "tlv320aic31xx-codec.2-0018",
		.params = &cht_dai_params_ssp2,
		.be_fixup = cht_codec_loop_fixup,
		.dsp_loopback = true,
	},
	{
		.name = "Cherrytrail Modem-Loop Port",
		.stream_name = "Cherrytrail Modem-Loop",
		.cpu_dai_name = "ssp0-port",
		.platform_name = "sst-platform",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.params = &cht_dai_params_ssp0,
		.be_fixup = cht_modem_fixup,
		.dsp_loopback = true,
	},
	{
		.name = "Cherrytrail BTFM-Loop Port",
		.stream_name = "Cherrytrail BTFM-Loop",
		.cpu_dai_name = "ssp1-port",
		.platform_name = "sst-platform",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.params = &cht_dai_params_ssp1_bt_nb,
		.be_fixup = cht_bt_fm_fixup,
		.dsp_loopback = true,
	},
		/* back ends */
	{
		.name = "SSP2-Codec",
		.be_id = 1,
		.cpu_dai_name = "ssp2-port",
		.platform_name = "sst-platform",
		.no_pcm = 1,
		.codec_dai_name = "tlv320aic31xx-codec",
		.codec_name = "tlv320aic31xx-codec.2-0018",
		.be_hw_params_fixup = cht_codec_fixup,
		.ignore_suspend = 1,
		.ops = &cht_be_ssp2_ops,
	},
	{
		.name = "SSP1-BTFM",
		.be_id = 2,
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.ignore_suspend = 1,
	},
	{
		.name = "SSP0-Modem",
		.be_id = 3,
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.no_pcm = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.ignore_suspend = 1,
	},
};

#ifdef CONFIG_PM_SLEEP
static int snd_cht_prepare(struct device *dev)
{
	pr_debug("In %s device name\n", __func__);
	return snd_soc_suspend(dev);
}

static void snd_cht_complete(struct device *dev)
{
	pr_debug("In %s\n", __func__);
	snd_soc_resume(dev);
}

static int snd_cht_poweroff(struct device *dev)
{
	pr_debug("In %s\n", __func__);
	return snd_soc_poweroff(dev);
}
#else
#define snd_cht_prepare NULL
#define snd_cht_complete NULL
#define snd_cht_poweroff NULL
#endif

/* SoC card */
static struct snd_soc_card snd_soc_card_cht = {
	.name = "cherrytrailaud",
	.dai_link = cht_dailink,
	.num_links = ARRAY_SIZE(cht_dailink),
	.set_bias_level = cht_set_bias_level,
	.dapm_widgets = cht_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cht_dapm_widgets),
	.dapm_routes = cht_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cht_audio_map),
};

static int snd_cht_mc_probe(struct platform_device *pdev)
{
	int ret_val = 0;
	struct cht_mc_private *drv;
	int codec_gpio;

	pr_debug("Entry %s\n", __func__);

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_ATOMIC);
	if (!drv) {
		pr_err("allocation failed\n");
		return -ENOMEM;
	}
	/* Reset pin for codec is GPIOS_21 */
	/* Codec jack detect direct line GPIOS_27 */
	/* PWM Vibrator pin is GPIOC_95 */
	/* GPIO_SUS4 */
	codec_gpio = acpi_get_gpio_by_index(&pdev->dev, 0, NULL);
	pr_debug("%s: GPIOs - codec %d", __func__, codec_gpio);
	hs_gpio.gpio = codec_gpio;

	drv->intr_debounce = CHT_INTR_DEBOUNCE;
	drv->hs_insert_det_delay = CHT_HS_INSERT_DET_DELAY;
	drv->hs_remove_det_delay = CHT_HS_REMOVE_DET_DELAY;
	drv->button_press_delay = CHT_BUTTON_PRESS_DELAY;
	drv->button_release_delay = CHT_BUTTON_RELEASE_DELAY;
	drv->hs_det_poll_intrvl = CHT_HS_DET_POLL_INTRVL;
	drv->hs_det_retry = CHT_HS_DET_RETRY_COUNT;
	drv->button_en_delay = CHT_BUTTON_EN_DELAY;
	drv->process_button_events = false;

	INIT_DELAYED_WORK(&drv->hs_insert_work, cht_check_hs_insert_status);
	INIT_DELAYED_WORK(&drv->hs_remove_work, cht_check_hs_remove_status);
	INIT_DELAYED_WORK(&drv->hs_button_press_work,
			 cht_check_hs_button_press_status);
	INIT_DELAYED_WORK(&drv->hs_button_release_work,
			 cht_check_hs_button_release_status);
	INIT_DELAYED_WORK(&drv->hs_button_en_work, cht_enable_hs_button_events);
	mutex_init(&drv->jack_mlock);

	/* register the soc card */
	snd_soc_card_cht.dev = &pdev->dev;
	snd_soc_card_set_drvdata(&snd_soc_card_cht, drv);

	ret_val = snd_soc_register_card(&snd_soc_card_cht);
	if (ret_val) {
		pr_err("snd_soc_register_card failed %d\n", ret_val);
		return ret_val;
	}
	platform_set_drvdata(pdev, &snd_soc_card_cht);
	pr_info("%s successful\n", __func__);
	return ret_val;
}

static void snd_cht_unregister_jack(struct cht_mc_private *ctx)
{
	/* Set process button events to false so that the button
	   delayed work will not be scheduled.*/
	ctx->process_button_events = false;
	cancel_delayed_work_sync(&ctx->hs_insert_work);
	cancel_delayed_work_sync(&ctx->hs_button_en_work);
	cancel_delayed_work_sync(&ctx->hs_button_press_work);
	cancel_delayed_work_sync(&ctx->hs_button_release_work);
	cancel_delayed_work_sync(&ctx->hs_remove_work);
	snd_soc_jack_free_gpios(&ctx->jack, 1, &hs_gpio);
}

static int snd_cht_mc_remove(struct platform_device *pdev)
{
	struct snd_soc_card *soc_card = platform_get_drvdata(pdev);
	struct cht_mc_private *drv = snd_soc_card_get_drvdata(soc_card);

	pr_debug("In %s\n", __func__);

	snd_cht_unregister_jack(drv);
	snd_soc_card_set_drvdata(soc_card, NULL);
	snd_soc_unregister_card(soc_card);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static void snd_cht_mc_shutdown(struct platform_device *pdev)
{
	struct snd_soc_card *soc_card = platform_get_drvdata(pdev);
	struct cht_mc_private *drv = snd_soc_card_get_drvdata(soc_card);

	pr_debug("In %s\n", __func__);
	snd_cht_unregister_jack(drv);
}

static const struct dev_pm_ops snd_cht_mc_pm_ops = {
	.prepare = snd_cht_prepare,
	.complete = snd_cht_complete,
	.poweroff = snd_cht_poweroff,
};

static const struct acpi_device_id cht_mc_acpi_ids[] = {
	{ "TIMC22A8", 0 },
	{},
};
MODULE_DEVICE_TABLE(acpi, cht_mc_acpi_ids);

static struct platform_driver snd_cht_mc_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "cht_aic31xx",
		.pm = &snd_cht_mc_pm_ops,
		.acpi_match_table = ACPI_PTR(cht_mc_acpi_ids),
	},
	.probe = snd_cht_mc_probe,
	.remove = snd_cht_mc_remove,
	.shutdown = snd_cht_mc_shutdown,
};

static int __init snd_cht_driver_init(void)
{
	int ret;
	ret = platform_driver_register(&snd_cht_mc_driver);
	if (ret)
		pr_err("Fail to register Cherrytrail Machine driver cht_aic31xx\n");
	else
		pr_info("Cherrytrail Machine Driver cht_aic31xx registerd\n");
	return ret;
}
late_initcall(snd_cht_driver_init);

static void __exit snd_cht_driver_exit(void)
{
	pr_debug("In %s\n", __func__);
	platform_driver_unregister(&snd_cht_mc_driver);
}
module_exit(snd_cht_driver_exit);

MODULE_DESCRIPTION("ASoC Intel(R) Cherrytrail CR Machine driver");
MODULE_AUTHOR("Dharageswari R <dharageswari.r@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:chtaic31xx-audio");
