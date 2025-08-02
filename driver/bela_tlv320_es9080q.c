// SPDX-License-Identifier: GPL-2.0
/*
* Bela Audio Cape Machine Driver
* Single sound card with TLV320AIC3104 + ES9080Q 
* 10 outputs (2 from AIC3104 + 8 from ES9080Q) + 2 inputs (from AIC3104)
*/

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

/* Define multiple supported sample rates instead of just 48kHz */
#define TDM_SLOTS          16
#define TDM_SLOT_WIDTH     32
#define BELA_OUTPUTS       10
#define BELA_INPUTS        2

struct bela_priv {
   struct snd_soc_card card;
   struct snd_soc_dai_link dai_link;
   struct snd_soc_dai_link_component cpu;
   struct snd_soc_dai_link_component codecs[2];  /* AIC3104 + ES9080Q */
   struct snd_soc_dai_link_component platform;
   struct clk *mclk;
   bool mclk_enabled;  /* Track MCLK state */
   bool bclk_started;  /* Track BCLK state */
   struct snd_soc_dai *cpu_dai_cache; /* Cache CPU DAI for BCLK control */
};

/* Expanded supported sample rates */
static const unsigned int bela_rates[] = {
   44100,
   48000,
   88200,
   96000,
};

static const struct snd_pcm_hw_constraint_list bela_rate_constraints = {
   .count = ARRAY_SIZE(bela_rates),
   .list = bela_rates,
};

/**
 * bela_get_mclk_rate - Get appropriate MCLK rate for the given sample rate
 * @rate: Sample rate in Hz
 *
 * Returns an appropriate master clock frequency for the given sample rate
 */
static unsigned int bela_get_mclk_rate(unsigned int rate)
{
   switch (rate) {
   case 44100:
   case 88200:
       return 22579200; /* 22.5792 MHz for 44.1kHz family */
   case 48000:
   case 96000:
   default:
       return 24576000; /* 24.576 MHz for 48kHz family */
   }
}

/**
 * bela_start_early_bclk - Initialize and start BCLK for ES9080Q setup
 * @priv: Private driver data structure
 *
 * The ES9080Q DAC requires a stable BCLK/FSYNC before I2C configuration.
 * This function configures the McASP interface in TDM mode and starts
 * the clocks running continuously for codec operation.
 *
 * Return: 0 on success, negative error code on failure
 */
static int bela_start_early_bclk(struct bela_priv *priv)
{
   struct snd_soc_dai *cpu_dai = priv->cpu_dai_cache;
   unsigned int default_rate = 48000;
   unsigned int mclk_freq;
   int ret;

   if (!cpu_dai || priv->bclk_started) {
       return 0;
   }

   dev_info(priv->card.dev, "Starting early BCLK for ES9080Q\n");

   /* Enable MCLK first */
   if (priv->mclk && !priv->mclk_enabled) {
       mclk_freq = bela_get_mclk_rate(default_rate);
       ret = clk_prepare_enable(priv->mclk);
       if (ret) {
           dev_err(priv->card.dev, "Failed to enable MCLK: %d\n", ret);
           return ret;
       }
       priv->mclk_enabled = true;
       mclk_freq = clk_get_rate(priv->mclk);
       dev_info(priv->card.dev, "MCLK enabled: %lu Hz\n", mclk_freq);
   }

   /* Enable McASP runtime PM */
   if (cpu_dai->dev) {
       pm_runtime_get_sync(cpu_dai->dev);
   }

   /* Configure McASP as TDM master */
   ret = snd_soc_dai_set_fmt(cpu_dai,
                             SND_SOC_DAIFMT_DSP_B |
                             SND_SOC_DAIFMT_IB_NF |        
                             SND_SOC_DAIFMT_CBP_CFP);
   if (ret < 0) {
       dev_err(priv->card.dev, "Failed to set CPU DAI format: %d\n", ret);
       goto err_pm_put;
   }

   /* Set sysclk to generate clocks */
   ret = snd_soc_dai_set_sysclk(cpu_dai, 0, mclk_freq, SND_SOC_CLOCK_OUT);
   if (ret < 0) {
       dev_err(priv->card.dev, "Failed to set CPU sysclk: %d\n", ret);
       goto err_pm_put;
   }

   /* Configure TDM slots */
   ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0xFFFF, 0xFFFF, TDM_SLOTS, TDM_SLOT_WIDTH);
   if (ret < 0) {
       dev_err(priv->card.dev, "Failed to set CPU TDM slots: %d\n", ret);
       goto err_pm_put;
   }

   /* Start McASP clocks by calling the driver ops directly if available */
   if (cpu_dai->driver && cpu_dai->driver->ops) {
       struct snd_pcm_substream dummy_substream = {
           .stream = SNDRV_PCM_STREAM_PLAYBACK,
       };
       struct snd_pcm_hw_params dummy_params;

       /* Initialize dummy params with default rate */
       snd_mask_none(hw_param_mask(&dummy_params, SNDRV_PCM_HW_PARAM_FORMAT));
       snd_mask_set(hw_param_mask(&dummy_params, SNDRV_PCM_HW_PARAM_FORMAT), 
                    SNDRV_PCM_FORMAT_S32_LE);
       hw_param_interval(&dummy_params, SNDRV_PCM_HW_PARAM_RATE)->min = default_rate;
       hw_param_interval(&dummy_params, SNDRV_PCM_HW_PARAM_RATE)->max = default_rate;
       hw_param_interval(&dummy_params, SNDRV_PCM_HW_PARAM_CHANNELS)->min = 2;
       hw_param_interval(&dummy_params, SNDRV_PCM_HW_PARAM_CHANNELS)->max = 2;

       if (cpu_dai->driver->ops->hw_params) {
           ret = cpu_dai->driver->ops->hw_params(&dummy_substream, &dummy_params, cpu_dai);
           if (ret < 0) {
               dev_warn(priv->card.dev, "Failed to start McASP hw_params: %d\n", ret);
           }
       }

       if (cpu_dai->driver->ops->trigger) {
           ret = cpu_dai->driver->ops->trigger(&dummy_substream, 
                                              SNDRV_PCM_TRIGGER_START, cpu_dai);
           if (ret < 0) {
               dev_warn(priv->card.dev, "Failed to trigger McASP: %d\n", ret);
           }
       }
   }

   priv->bclk_started = true;
   dev_info(priv->card.dev, "Early BCLK started successfully\n");
   
   /* Give it some time to stabilize */
   msleep(50);
   
   return 0;

err_pm_put:
   if (cpu_dai->dev)
       pm_runtime_put_sync(cpu_dai->dev);
   if (priv->mclk && priv->mclk_enabled) {
       clk_disable_unprepare(priv->mclk);
       priv->mclk_enabled = false;
   }
   return ret;
}

/**
 * bela_startup - Called when a PCM stream is opened
 * @substream: PCM substream that is starting
 *
 * This function handles PCM stream startup operations:
 * - Ensures BCLK is running by calling bela_start_early_bclk if needed
 * - Sets up sample rate constraints (limited to 48kHz)
 * - Configures channel constraints based on stream direction:
 *   - Up to 10 channels for playback (combined AIC3104 + ES9080Q)
 *   - Up to 2 channels for capture (AIC3104 only)
 *
 * Return: 0 on success, negative error code on failure
 */
static int bela_startup(struct snd_pcm_substream *substream)
{
   struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
   struct snd_pcm_runtime *runtime = substream->runtime;
   struct snd_soc_card *card = rtd->card;
   struct bela_priv *priv = snd_soc_card_get_drvdata(card);
   int ret;

   dev_info(rtd->dev, "Bela startup: %s\n", 
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");

   /* Ensure early BCLK is running */
   if (!priv->bclk_started) {
       ret = bela_start_early_bclk(priv);
       if (ret) {
           dev_err(rtd->dev, "Failed to start early BCLK: %d\n", ret);
           return ret;
       }
   }

   ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
                                    &bela_rate_constraints);
   if (ret < 0)
       return ret;

   /* Set channel constraints based on stream direction */
   if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
       /* Now that ES9080Q is available, enable full 10 channels */
       runtime->hw.channels_min = 1;
       runtime->hw.channels_max = BELA_OUTPUTS;  // 10 channels
       
       ret = snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_CHANNELS,
                                          1, BELA_OUTPUTS);
       if (ret < 0) {
           dev_err(rtd->dev, "Failed to set channel constraint: %d\n", ret);
           return ret;
       }
       
       dev_info(rtd->dev, "Playback: configured for 1-%d channels\n", BELA_OUTPUTS);
   } else {
       /* 2 input channels from AIC3104 only */
       runtime->hw.channels_min = 1;
       runtime->hw.channels_max = BELA_INPUTS;
       
       ret = snd_pcm_hw_constraint_minmax(runtime, SNDRV_PCM_HW_PARAM_CHANNELS,
                                          1, BELA_INPUTS);
       if (ret < 0) {
           dev_err(rtd->dev, "Failed to set channel constraint: %d\n", ret);
           return ret;
       }
       
       dev_info(rtd->dev, "Capture: configured for 1-%d channels\n", BELA_INPUTS);
   }

   return 0;
}

/**
 * bela_hw_params - Configure hardware parameters for a PCM stream
 * @substream: PCM substream to configure
 * @params: Hardware parameters to apply
 *
 * This function configures both codecs based on the requested audio parameters:
 * - Ensures early BCLK is running
 * - Identifies and configures the AIC3104 codec for slots 0-1
 * - Identifies and configures the ES9080Q codec for slots 2-9 (when needed)
 * - Sets up TDM slot allocations, clock signals, and formats
 * - Handles multi-codec synchronization for unified operation
 *
 * Return: 0 on success, negative error code on failure
 */
static int bela_hw_params(struct snd_pcm_substream *substream,
                        struct snd_pcm_hw_params *params)
{
   struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
   struct snd_soc_card *card = rtd->card;
   struct bela_priv *priv = snd_soc_card_get_drvdata(card);
   struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
   struct snd_soc_dai *codec_dai;
   unsigned int rate = params_rate(params);
   unsigned int channels = params_channels(params);
   bool use_es9080q = (channels > 2);
   int ret, i;
   struct snd_soc_dai *aic3104_dai = NULL;
   struct snd_soc_dai *es9080q_dai = NULL;
   unsigned int bclk_freq;
   unsigned int mclk_freq;

   dev_info(card->dev, "Bela HW params: rate=%u, channels=%u, stream=%s\n",
            rate, channels, 
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");

   /* Get appropriate MCLK frequency for this sample rate */
   mclk_freq = bela_get_mclk_rate(rate);

   /* MCLK should already be enabled from early BCLK start */
   if (priv->mclk && priv->mclk_enabled) {
       unsigned int current_mclk = clk_get_rate(priv->mclk);
       
       /* If MCLK rate needs to change based on sample rate */
       if (current_mclk != mclk_freq) {
           dev_info(card->dev, "Changing MCLK from %u to %u Hz for rate %u\n", 
                    current_mclk, mclk_freq, rate);
           
           /* Disable BCLK first if running */
           if (priv->bclk_started && cpu_dai) {
               /* Stop BCLK - implementation depends on your driver */
               priv->bclk_started = false;
           }
           
           /* Disable then re-enable MCLK at new rate */
           clk_disable_unprepare(priv->mclk);
           clk_set_rate(priv->mclk, mclk_freq);
           ret = clk_prepare_enable(priv->mclk);
           if (ret) {
               dev_err(card->dev, "Failed to enable MCLK at new rate: %d\n", ret);
               return ret;
           }
           
           /* Re-start BCLK after MCLK change */
           ret = bela_start_early_bclk(priv);
           if (ret) {
               dev_err(card->dev, "Failed to restart BCLK: %d\n", ret);
               return ret;
           }
       }
       
       mclk_freq = clk_get_rate(priv->mclk);
       dev_info(card->dev, "MCLK frequency: %u Hz\n", mclk_freq);
   }

   /* Calculate BCLK frequency for TDM */
   bclk_freq = rate * TDM_SLOTS * TDM_SLOT_WIDTH;
   dev_info(card->dev, "BCLK frequency: %u Hz\n", bclk_freq);

   /* McASP should already be configured from early start, just verify */
   if (!priv->bclk_started) {
       dev_warn(card->dev, "BCLK not started early, starting now\n");
       ret = bela_start_early_bclk(priv);
       if (ret) {
           dev_err(card->dev, "Failed to start BCLK: %d\n", ret);
           return ret;
       }
   }

   /* First pass - identify codec DAIs */
   for_each_rtd_codec_dais(rtd, i, codec_dai) {
       if (strstr(codec_dai->name, "tlv320aic3x")) {
           aic3104_dai = codec_dai;
       } else if (strstr(codec_dai->name, "es9080q")) {
           es9080q_dai = codec_dai;
       }
   }

   /* Configure TLV320AIC3104 as slave */
   if (aic3104_dai) {
       dev_info(card->dev, "Configuring TLV320AIC3104 as TDM slave\n");

       ret = snd_soc_dai_set_fmt(aic3104_dai,
                                 SND_SOC_DAIFMT_DSP_B |
                                 SND_SOC_DAIFMT_IB_NF |        
                                 SND_SOC_DAIFMT_CBC_CFC);  // Codec is slave
       if (ret < 0) {
           dev_err(card->dev, "Failed to set AIC3104 DAI format: %d\n", ret);
           return ret;
       }

       /* AIC3104 uses MCLK as sysclk */
       ret = snd_soc_dai_set_sysclk(aic3104_dai, 0, mclk_freq, SND_SOC_CLOCK_IN);
       if (ret < 0) {
           dev_warn(card->dev, "Failed to set AIC3104 sysclk: %d\n", ret);
       } else {
           dev_info(card->dev, "AIC3104 sysclk set to %u Hz\n", mclk_freq);
       }

       ret = snd_soc_dai_set_tdm_slot(aic3104_dai, 0x0003, 0x0003, TDM_SLOTS, TDM_SLOT_WIDTH);
       if (ret < 0) {
           dev_err(card->dev, "Failed to set AIC3104 TDM slots: %d\n", ret);
           return ret;
       }

       dev_info(card->dev, "AIC3104 configured: slots 0-1, BCLK/FSYNC slave\n");
   }

   /* Configure ES9080Q as slave - BCLK is already running */
   if (es9080q_dai && use_es9080q) {
       dev_info(card->dev, "Configuring ES9080Q as TDM slave (BCLK active)\n");

       ret = snd_soc_dai_set_fmt(es9080q_dai,
                                 SND_SOC_DAIFMT_DSP_B |
                                 SND_SOC_DAIFMT_IB_NF |        
                                 SND_SOC_DAIFMT_CBC_CFC);  // Codec is slave
       if (ret < 0) {
           dev_warn(card->dev, "Failed to set ES9080Q DAI format: %d (continuing anyway)\n", ret);
       } else {
           dev_info(card->dev, "ES9080Q DAI format set successfully\n");
       }

       /* Set TDM slots for ES9080Q */
       ret = snd_soc_dai_set_tdm_slot(es9080q_dai, 0x03FC, 0x0000, 
                                      TDM_SLOTS, TDM_SLOT_WIDTH);
       if (ret < 0 && ret != -ENOTSUPP && ret != -ENOSYS) {
           dev_warn(card->dev, "Failed to set ES9080Q TDM slots: %d (continuing anyway)\n", ret);
       } else if (ret == -ENOTSUPP || ret == -ENOSYS) {
           dev_info(card->dev, "ES9080Q does not support TDM slot configuration (using default mapping)\n");
       } else {
           dev_info(card->dev, "ES9080Q TDM slots set successfully (slots 2-9)\n");
       }

       /* Set sysclk for ES9080Q */
       ret = snd_soc_dai_set_sysclk(es9080q_dai, 0, bclk_freq, SND_SOC_CLOCK_IN);
       if (ret < 0) {
           dev_warn(card->dev, "Failed to set ES9080Q sysclk: %d (continuing anyway)\n", ret);
       } else {
           dev_info(card->dev, "ES9080Q sysclk set to %u Hz\n", bclk_freq);
       }

       dev_info(card->dev, "ES9080Q configured: slots 2-9, BCLK/FSYNC slave\n");
   }

   dev_info(card->dev, "=== HW PARAMS COMPLETE ===\n");
   dev_info(card->dev, "P9_25 (MCLK): %u Hz\n", mclk_freq);
   dev_info(card->dev, "P9_31 (BCLK): Active at %u Hz\n", bclk_freq);
   dev_info(card->dev, "P9_29 (LRCLK): %u Hz\n", rate);
   
   return 0;
}

/**
 * bela_shutdown - Called when a PCM stream is closed
 * @substream: PCM substream that is shutting down
 *
 * Keep BCLK running to avoid ES9080Q re-initialization issues
 */
static void bela_shutdown(struct snd_pcm_substream *substream)
{
   struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
   struct snd_soc_card *card = rtd->card;

   dev_info(rtd->dev, "Bela shutdown - keeping BCLK active for ES9080Q\n");
   
   /* Don't stop BCLK to avoid ES9080Q initialization issues on next playback */
   /* The BCLK will only be stopped when the driver is removed */
}

static const struct snd_soc_ops bela_ops = {
   .startup = bela_startup,
   .hw_params = bela_hw_params,
   .shutdown = bela_shutdown,
};

/* DAPM widgets for unified sound card */
static const struct snd_soc_dapm_widget bela_dapm_widgets[] = {
   /* AIC3104 outputs (channels 1-2) */
   SND_SOC_DAPM_HP("Headphone Jack", NULL),
   SND_SOC_DAPM_LINE("AIC3104 Line Out", NULL),
   
   /* AIC3104 inputs */
   SND_SOC_DAPM_LINE("Line In", NULL),
   SND_SOC_DAPM_MIC("Mic In", NULL),
   
   /* ES9080Q outputs (channels 3-10) */
   SND_SOC_DAPM_OUTPUT("DAC Out 1"),
   SND_SOC_DAPM_OUTPUT("DAC Out 2"),
   SND_SOC_DAPM_OUTPUT("DAC Out 3"),
   SND_SOC_DAPM_OUTPUT("DAC Out 4"),
   SND_SOC_DAPM_OUTPUT("DAC Out 5"),
   SND_SOC_DAPM_OUTPUT("DAC Out 6"),
   SND_SOC_DAPM_OUTPUT("DAC Out 7"),
   SND_SOC_DAPM_OUTPUT("DAC Out 8"),
};

/* Audio routing */
static const struct snd_soc_dapm_route bela_dapm_routes[] = {
   /* AIC3104 routing */
   {"Headphone Jack", NULL, "HPLOUT"},
   {"Headphone Jack", NULL, "HPROUT"},
   {"AIC3104 Line Out", NULL, "LLOUT"},
   {"AIC3104 Line Out", NULL, "RLOUT"},
   {"LINE1L", NULL, "Line In"},
   {"LINE1R", NULL, "Line In"},
   
   /* ES9080Q routing */
   {"DAC Out 1", NULL, "Playback"},
   {"DAC Out 2", NULL, "Playback"},
   {"DAC Out 3", NULL, "Playback"},
   {"DAC Out 4", NULL, "Playback"},
   {"DAC Out 5", NULL, "Playback"},
   {"DAC Out 6", NULL, "Playback"},
   {"DAC Out 7", NULL, "Playback"},
   {"DAC Out 8", NULL, "Playback"},
};

/* Controls */
static const struct snd_kcontrol_new bela_controls[] = {
   /* AIC3104 controls */
   SOC_DAPM_PIN_SWITCH("Headphone Jack"),
   SOC_DAPM_PIN_SWITCH("AIC3104 Line Out"),
   SOC_DAPM_PIN_SWITCH("Line In"),
   SOC_DAPM_PIN_SWITCH("Mic In"),
   
   /* ES9080Q controls */
   SOC_DAPM_PIN_SWITCH("DAC Out 1"),
   SOC_DAPM_PIN_SWITCH("DAC Out 2"),
   SOC_DAPM_PIN_SWITCH("DAC Out 3"),
   SOC_DAPM_PIN_SWITCH("DAC Out 4"),
   SOC_DAPM_PIN_SWITCH("DAC Out 5"),
   SOC_DAPM_PIN_SWITCH("DAC Out 6"),
   SOC_DAPM_PIN_SWITCH("DAC Out 7"),
   SOC_DAPM_PIN_SWITCH("DAC Out 8"),
};

static int bela_init(struct snd_soc_pcm_runtime *rtd)
{
   struct snd_soc_card *card = rtd->card;
   struct bela_priv *priv = snd_soc_card_get_drvdata(card);
   struct snd_soc_dai *codec_dai;
   struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
   int i, ret;

   dev_info(card->dev, "Bela unified sound card initialization\n");

   /* Cache CPU DAI for early BCLK start */
   priv->cpu_dai_cache = cpu_dai;

   /* Log all available codecs and their capabilities */
   for_each_rtd_codec_dais(rtd, i, codec_dai) {
       dev_info(card->dev, "Found codec DAI [%d]: %s\n", i, codec_dai->name);
       if (codec_dai->driver) {
           if (codec_dai->driver->playback.channels_max)
               dev_info(card->dev, "  Playback: %d-%d channels\n",
                        codec_dai->driver->playback.channels_min,
                        codec_dai->driver->playback.channels_max);
           if (codec_dai->driver->capture.channels_max)
               dev_info(card->dev, "  Capture: %d-%d channels\n",
                        codec_dai->driver->capture.channels_min,
                        codec_dai->driver->capture.channels_max);
       }
   }

   /* Start early BCLK for ES9080Q */
   ret = bela_start_early_bclk(priv);
   if (ret) {
       dev_err(card->dev, "Failed to start early BCLK: %d\n", ret);
       /* Continue anyway, it might work later */
   }

   /* Enable all pins by default */
   snd_soc_dapm_enable_pin(&card->dapm, "Headphone Jack");
   snd_soc_dapm_enable_pin(&card->dapm, "AIC3104 Line Out");
   snd_soc_dapm_enable_pin(&card->dapm, "Line In");
   snd_soc_dapm_enable_pin(&card->dapm, "Mic In");
   
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 1");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 2");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 3");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 4");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 5");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 6");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 7");
   snd_soc_dapm_enable_pin(&card->dapm, "DAC Out 8");

   dev_info(card->dev, "Bela unified card ready: %d outputs + %d inputs\n", 
            BELA_OUTPUTS, BELA_INPUTS);
   return 0;
}

/**
 * bela_probe - Driver probe function called on device detection
 * @pdev: Platform device to probe
 *
 * This function handles driver initialization when the device is found:
 * - Allocates private data structure
 * - Parses device tree nodes for CPU DAI and codec components
 * - Sets up sound card, DAI links, and component bindings
 * - Registers the unified multi-codec sound card with ALSA
 *
 * The driver creates a single sound card with multiple codecs
 * using a shared TDM bus for synchronized multi-channel audio.
 *
 * Return: 0 on success, negative error code on failure
 */
static int bela_probe(struct platform_device *pdev)
{
   struct device *dev = &pdev->dev;
   struct device_node *np = dev->of_node;
   struct device_node *cpu_node, *aic3104_node, *es9080q_node;
   struct bela_priv *priv;
   int ret;

   if (!np) {
       dev_err(dev, "Device tree node not found\n");
       return -ENODEV;
   }

   priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
   if (!priv)
       return -ENOMEM;

   /* Initialize state */
   priv->mclk_enabled = false;
   priv->bclk_started = false;

   /* Parse device tree */
   cpu_node = of_parse_phandle(np, "cpu-dai", 0);
   aic3104_node = of_parse_phandle(np, "aic3104-codec", 0);
   es9080q_node = of_parse_phandle(np, "es9080q-codec", 0);
   
   if (!cpu_node || !aic3104_node || !es9080q_node) {
       dev_err(dev, "Missing CPU or codec DAI in device tree\n");
       ret = -EINVAL;
       goto err_put_nodes;
   }

   /* Initialize card - single unified card */
   priv->card.owner = THIS_MODULE;
   priv->card.dev = dev;
   priv->card.name = "Bela";
   priv->card.long_name = "Bela Audio Cape - Unified 10+2 Channel";
   priv->card.dai_link = &priv->dai_link;
   priv->card.num_links = 1;  /* Single DAI link with multiple codecs */
   priv->card.dapm_widgets = bela_dapm_widgets;
   priv->card.num_dapm_widgets = ARRAY_SIZE(bela_dapm_widgets);
   priv->card.dapm_routes = bela_dapm_routes;
   priv->card.num_dapm_routes = ARRAY_SIZE(bela_dapm_routes);
   priv->card.controls = bela_controls;
   priv->card.num_controls = ARRAY_SIZE(bela_controls);

   /* Setup components */
   priv->cpu.of_node = cpu_node;
   priv->platform.of_node = cpu_node;
   
   /* Setup codec components - both AIC3104 and ES9080Q in single link */
   priv->codecs[0].of_node = aic3104_node;
   priv->codecs[0].dai_name = "tlv320aic3x-hifi";
   
   priv->codecs[1].of_node = es9080q_node;
   priv->codecs[1].dai_name = "es9080q-hifi";

   /* Setup single DAI link with multiple codecs */
   priv->dai_link.name = "Bela Unified TDM";
   priv->dai_link.stream_name = "Bela Multi-Channel Audio";
   priv->dai_link.cpus = &priv->cpu;
   priv->dai_link.num_cpus = 1;
   priv->dai_link.codecs = priv->codecs;
   priv->dai_link.num_codecs = 2;  /* Both AIC3104 and ES9080Q */
   priv->dai_link.platforms = &priv->platform;
   priv->dai_link.num_platforms = 1;
   priv->dai_link.ops = &bela_ops;
   priv->dai_link.init = bela_init;

   /* Get MCLK clock */
   priv->mclk = devm_clk_get(dev, "mclk");
   if (IS_ERR(priv->mclk)) {
       ret = PTR_ERR(priv->mclk);
       dev_err(dev, "Failed to get MCLK: %d\n", ret);
       goto err_put_nodes;
   }

   snd_soc_card_set_drvdata(&priv->card, priv);

   dev_info(dev, "Registering unified Bela sound card (%d outputs + %d inputs)\n", 
            BELA_OUTPUTS, BELA_INPUTS);

   ret = devm_snd_soc_register_card(dev, &priv->card);
   if (ret) {
       if (ret == -EPROBE_DEFER) {
           dev_info(dev, "Deferred - codecs not ready yet\n");
       } else {
           dev_err(dev, "Failed to register sound card: %d\n", ret);
       }
       goto err_put_nodes;
   }

   dev_info(dev, "Bela unified sound card registered successfully!\n");
   dev_info(dev, "Early BCLK enabled for ES9080Q stability\n");

   return 0;
       
err_put_nodes:
   of_node_put(cpu_node);
   of_node_put(aic3104_node);
   of_node_put(es9080q_node);
   return ret;
}

/**
 * bela_remove - Driver removal callback
 * @pdev: Platform device being removed
 *
 * Performs clean shutdown of hardware when driver is removed:
 * - Stops BCLK (serial clock) if running
 * - Disables MCLK (master clock)
 * - Performs any other necessary cleanup
 */
static void bela_remove(struct platform_device *pdev)
{
   struct bela_priv *priv = platform_get_drvdata(pdev);

   /* Stop BCLK if running */
   if (priv->cpu_dai_cache && priv->bclk_started) {
       struct snd_soc_dai *cpu_dai = priv->cpu_dai_cache;
       
       if (cpu_dai->driver && cpu_dai->driver->ops && cpu_dai->driver->ops->trigger) {
           struct snd_pcm_substream dummy_substream = {
               .stream = SNDRV_PCM_STREAM_PLAYBACK,
           };
           cpu_dai->driver->ops->trigger(&dummy_substream, 
                                        SNDRV_PCM_TRIGGER_STOP, cpu_dai);
       }
       
       if (cpu_dai->dev) {
           pm_runtime_put_sync(cpu_dai->dev);
       }
   }

   /* Disable MCLK if it's still enabled */
   if (priv->mclk && priv->mclk_enabled) {
       clk_disable_unprepare(priv->mclk);
       priv->mclk_enabled = false;
   }

   dev_info(&pdev->dev, "Bela unified sound card removed\n");
}

/* 
 * Device Tree compatible string matching table
 * The driver will be instantiated for any device tree node
 * with the compatible string "bela,audio-cape"
 */
static const struct of_device_id bela_of_match[] = {
   { .compatible = "bela,audio-cape" },
   { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bela_of_match);

/*
 * Platform driver definition
 * This connects our probe/remove functions with the
 * kernel's platform device framework
 */
static struct platform_driver bela_driver = {
   .probe = bela_probe,
   .remove_new = bela_remove,
   .driver = {
       .name = "bela-audio-cape",            /* Driver name */
       .of_match_table = bela_of_match,      /* Device tree matching */
   },
};

/* Register our platform driver with the kernel */
module_platform_driver(bela_driver);

/*
 * Bela Audio Cape Multi-Channel Driver for Bela RevC hardware.
 * Supports integration with TLV320 audio codec and ES9080Q DAC.
 * Enables advanced audio features and multi-channel capabilities for Bela RevC.
 */
MODULE_DESCRIPTION("Bela Audio Cape Multi-Channel Driver");
MODULE_AUTHOR("Jian De <jiande2020@gmail.com>");
MODULE_AUTHOR("Giulio Moro");
MODULE_LICENSE("GPL");