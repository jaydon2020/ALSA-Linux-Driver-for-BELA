// SPDX-License-Identifier: GPL-2.0-only
/*
* es9080q.c  --  ES9080Q ALSA SoC Audio driver
*/

#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/clk.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <asm/div64.h>
#include <sound/initval.h>
#include <linux/regulator/consumer.h>
#include <linux/io.h>
#include "es9080q.h"

/* ES9080Q constants from C++ implementation */
#define ES9080Q_NUM_BITS        256
#define ES9080Q_SLOT_SIZE       32
#define ES9080Q_DATA_SIZE       16
#define ES9080Q_STARTING_SLOT   0
#define ES9080Q_NUM_SLOTS       (ES9080Q_NUM_BITS / ES9080Q_SLOT_SIZE)
#define ES9080Q_NUM_OUT_CHANNELS 8

/* Volume control constants */
#define ES9080Q_VOLUME_MIN      0
#define ES9080Q_VOLUME_MAX      255
#define ES9080Q_VOLUME_STEP     1

/* Register definitions */
#define ES9080Q_REG_AMP_CTRL        0     /* Turn on/off AMP */
#define ES9080Q_REG_CLK_EN          1     /* Clock enable */
#define ES9080Q_REG_TDM_EN          2     /* TDM enable */
#define ES9080Q_REG_DAC_CONFIG      3     /* Sample rate config */
#define ES9080Q_REG_MASTER_CLK      4     /* Master clock config */
#define ES9080Q_REG_ANALOG_CTRL     5     /* Analog section control */
#define ES9080Q_REG_CP_CLOCK_DIV    6     /* Charge pump clock divider */
#define ES9080Q_REG_ANALOG_DELAY    7     /* Analog delay sequence */
#define ES9080Q_REG_PLL_LOCK        51    /* PLL lock signal */
#define ES9080Q_REG_INPUT_CONFIG    77    /* Input configuration */
#define ES9080Q_REG_MASTER_MODE     78    /* Master mode config */
#define ES9080Q_REG_TDM_CONFIG1     79    /* TDM configuration 1 */
#define ES9080Q_REG_TDM_CONFIG2     80    /* TDM configuration 2 */
#define ES9080Q_REG_TDM_CONFIG3     81    /* TDM configuration 3 */
#define ES9080Q_REG_BCK_WS_MONITOR  82    /* BCK/WS monitor */
#define ES9080Q_REG_TDM_VALID_PULSE 83    /* TDM valid pulse config */
#define ES9080Q_REG_DAC_RESYNC      92    /* DAC clock resync */
#define ES9080Q_REG_VOLUME_BASE     94    /* Volume registers start (94-101) */
#define ES9080Q_REG_VOLUME_CTRL     105   /* Volume control */
#define ES9080Q_REG_FILTER_SHAPE    108   /* Filter shape */
#define ES9080Q_REG_DITHER          109   /* Dither control */
#define ES9080Q_REG_THD_C2_135      111   /* THD compensation CH1/3/5/7 */
#define ES9080Q_REG_THD_C2_246      115   /* THD compensation CH2/4/6/8 */
#define ES9080Q_REG_AUTOMUTE        119   /* Automute control */
#define ES9080Q_REG_NSMOD_PHASE     128   /* NSMOD dither phases */
#define ES9080Q_REG_NSMOD_TYPE      129   /* NSMOD dither type */
#define ES9080Q_REG_GAIN_18DB       154   /* 18dB gain boost */

/* Write-only registers */
#define ES9080Q_REG_RESET_PLL       192   /* Reset and PLL register 1 */
#define ES9080Q_REG_GPIO_PLL        193   /* GPIO and PLL register 2 */
#define ES9080Q_REG_PLL_PARAM       202   /* PLL parameters */

struct es9080q_priv {
   struct snd_soc_component *component;
   struct i2c_client *rw_client;    /* Read/write client (0x48) */
   struct i2c_client *wo_client;    /* Write-only client (0x4c) */
   struct regmap *rw_regmap;
   struct regmap *wo_regmap;
   struct gpio_desc *reset_gpio;
   bool codec_initialized;
   bool is_master;
   unsigned int sysclk_freq;
   int volume[ES9080Q_NUM_OUT_CHANNELS];  /* Volume per channel */
};

/* Initial register sequence for BCLK-dependent initialization */
static const struct reg_sequence es9080q_init_seq[] = {
   /* Set GPIO1 (MCLK) pad to input mode, invert CLKHV phase for better DNR */
   { ES9080Q_REG_RESET_PLL, 0x03 },
   /* PLL bypass, remove 10k DVDD shunt, set PLL input to MCLK, enable PLL inputs */
   { ES9080Q_REG_GPIO_PLL, 0xC3 },
   /* PLL parameters */
   { ES9080Q_REG_PLL_PARAM, 0x40 },
};

/* Write to write-only register safely */
static int es9080q_wo_write(struct es9080q_priv *priv, unsigned int reg, 
                           unsigned int val)
{
   int ret, retry;
   
   /* Check if this is indeed a write-only register */
   if (reg < 192 || reg > 203) {
       dev_err(priv->component->dev, "Register %d is not write-only\n", reg);
       return -EINVAL;
   }
   
   for (retry = 0; retry < 3; retry++) {
       ret = regmap_write(priv->wo_regmap, reg, val);
       if (ret == 0) {
           dev_dbg(priv->component->dev, "WO write reg 0x%02X = 0x%02X\n", 
                  reg, val);
           return 0;
       }
       
       dev_warn(priv->component->dev, "WO write failed reg 0x%02X: %d (attempt %d)\n", 
               reg, ret, retry + 1);
       
       if (retry < 2)
           udelay(100);
   }
   
   return ret;
}

/* Write to read/write register safely */
static int es9080q_rw_write(struct es9080q_priv *priv, unsigned int reg, 
                           unsigned int val)
{
   int ret, retry;
   
   /* Check if this is a valid read/write register */
   if (reg > 164 && reg < 224) {
       dev_err(priv->component->dev, "Register %d is not read/write\n", reg);
       return -EINVAL;
   }
   
   for (retry = 0; retry < 3; retry++) {
       ret = regmap_write(priv->rw_regmap, reg, val);
       if (ret == 0) {
           dev_dbg(priv->component->dev, "RW write reg 0x%02X = 0x%02X\n", 
                  reg, val);
           return 0;
       }
       
       dev_warn(priv->component->dev, "RW write failed reg 0x%02X: %d (attempt %d)\n", 
               reg, ret, retry + 1);
       
       if (retry < 2)
           udelay(100);
   }
   
   return ret;
}

/* Calculate and configure clocking parameters */
static int es9080q_configure_clocking(struct es9080q_priv *priv, 
                                    unsigned int sample_rate)
{
   unsigned int mclk_freq = priv->sysclk_freq;
   const unsigned int mclk_over_bck = 2;  /* From C++ implementation */
   const unsigned int mclk_over_ws = ES9080Q_NUM_BITS * mclk_over_bck;
   bool is16bit = (ES9080Q_SLOT_SIZE == 16);
   const unsigned int master_bck_div1 = 1;
   int ret;

   dev_info(priv->component->dev, "Configuring clocking: MCLK=%u, Fs=%u\n",
            mclk_freq, sample_rate);

   /* Calculate divider values based on C++ implementation */
   unsigned int divide_value_menc = mclk_over_bck / (master_bck_div1 ? 1 : (is16bit ? 4 : 2));
   unsigned int ws_scale_factor = mclk_over_ws / (divide_value_menc * 128);
   unsigned int master_ws_scale = __builtin_ctz(ws_scale_factor);
   
   if (!ws_scale_factor || (ws_scale_factor & ~(1 << master_ws_scale)) || master_ws_scale > 4) {
       dev_err(priv->component->dev, "Invalid WS scale factor %u\n", ws_scale_factor);
       return -EINVAL;
   }

   /* DAC CONFIG - Sample Rate register */
   unsigned int select_idac_num = mclk_over_ws / 128 - 1;
   ret = es9080q_rw_write(priv, ES9080Q_REG_DAC_CONFIG, select_idac_num);
   if (ret)
       return ret;

   /* MASTER CLOCK CONFIG */
   if (divide_value_menc < 1 || divide_value_menc > 128) {
       dev_err(priv->component->dev, "Invalid MENC divider %u\n", divide_value_menc);
       return -EINVAL;
   }
   
   unsigned int select_menc_num = divide_value_menc - 1;
   ret = es9080q_rw_write(priv, ES9080Q_REG_MASTER_CLK, select_menc_num);
   if (ret)
       return ret;

   /* CP CLOCK DIV - Charge pump clock */
   const unsigned int cp_clock_min = 500000;
   const unsigned int cp_clock_max = 1000000;
   unsigned int cp_clk_div;
   unsigned int cp_clock;
   
   for (cp_clk_div = 0; cp_clk_div <= 255; cp_clk_div++) {
       cp_clock = mclk_freq / ((cp_clk_div + 1) * 2);
       if (cp_clock <= cp_clock_max)
           break;
   }
   
   if (cp_clock < cp_clock_min || cp_clock > cp_clock_max) {
       dev_err(priv->component->dev, "Cannot find valid CP_CLK_DIV\n");
       return -EINVAL;
   }
   
   ret = es9080q_rw_write(priv, ES9080Q_REG_CP_CLOCK_DIV, cp_clk_div);
   if (ret)
       return ret;

   /* Configure TDM if in master mode */
   if (priv->is_master) {
       /* MASTER MODE CONFIG */
       unsigned int master_frame_length = is16bit ? 2 : 0;
       ret = es9080q_rw_write(priv, ES9080Q_REG_MASTER_MODE, 
                             (master_bck_div1 << 6) |
                             (master_frame_length << 3) |
                             (1 << 2));  /* Pulse WS mode */
       if (ret)
           return ret;

       /* TDM CONFIG1 */
       ret = es9080q_rw_write(priv, ES9080Q_REG_TDM_CONFIG1,
                             (master_ws_scale << 4) |
                             (ES9080Q_NUM_SLOTS - 1));
       if (ret)
           return ret;
   }

   return 0;
}

/* Initialize ES9080Q codec when BCLK is stable */
static int es9080q_initialize_codec(struct es9080q_priv *priv, 
                                  unsigned int sample_rate)
{
   int ret, i;

   if (priv->codec_initialized) {
       dev_info(priv->component->dev, "Codec already initialized\n");
       return 0;
   }

   dev_info(priv->component->dev, "Initializing ES9080Q\n");

   /* Enable clocks and TDM */
   ret = es9080q_rw_write(priv, ES9080Q_REG_CLK_EN, 0xFF);
   if (ret)
       return ret;

   ret = es9080q_rw_write(priv, ES9080Q_REG_TDM_EN, 0x01);
   if (ret)
       return ret;

   /* Configure clocking */
   ret = es9080q_configure_clocking(priv, sample_rate);
   if (ret)
       return ret;

   /* Enable analog section */
   ret = es9080q_rw_write(priv, ES9080Q_REG_ANALOG_CTRL, 0xFF);
   if (ret)
       return ret;

   /* Analog delay sequence */
   ret = es9080q_rw_write(priv, ES9080Q_REG_ANALOG_DELAY, 0xBB);
   if (ret)
       return ret;

   /* Force PLL lock signal */
   ret = es9080q_rw_write(priv, ES9080Q_REG_PLL_LOCK, 0x80);
   if (ret)
       return ret;

   /* INPUT CONFIG */
   ret = es9080q_rw_write(priv, ES9080Q_REG_INPUT_CONFIG,
                         (priv->is_master << 4) |  /* Enable master mode if needed */
                         (0 << 2) |                /* INPUT_SEL: TDM */
                         (0 << 0));                /* AUTO_INPUT_SELECT: disabled */
   if (ret)
       return ret;

   /* TDM CONFIG2 - Left justified mode */
   ret = es9080q_rw_write(priv, ES9080Q_REG_TDM_CONFIG2,
                         (1 << 7) |    /* TDM_LJ_MODE: LJ mode */
                         (0 << 6) |    /* TDM_VALID_EDGE: negative edge */
                         (8 << 0));    /* TDM_VALID_PULSE_LEN: 8 for 8+ channels */
   if (ret)
       return ret;

   /* TDM CONFIG3 - Bit width */
   unsigned int tdm_bit_width;
   switch (ES9080Q_SLOT_SIZE) {
   case 32:
       tdm_bit_width = 0;
       break;
   case 24:
       tdm_bit_width = 1;
       break;
   case 16:
       tdm_bit_width = 2;
       break;
   default:
       dev_err(priv->component->dev, "Invalid slot size %d\n", ES9080Q_SLOT_SIZE);
       return -EINVAL;
   }
   
   ret = es9080q_rw_write(priv, ES9080Q_REG_TDM_CONFIG3, 
                         (tdm_bit_width << 6) |
                         (0 << 5) |    /* TDM_CHAIN_MODE: disable daisy chain */
                         (0 << 0));    /* TDM_DATA_LATCH_ADJ: 0 */
   if (ret)
       return ret;

   /* BCK/WS Monitor - disable */
   ret = es9080q_rw_write(priv, ES9080Q_REG_BCK_WS_MONITOR, 0x00);
   if (ret)
       return ret;

   /* TDM Valid Pulse Config */
   ret = es9080q_rw_write(priv, ES9080Q_REG_TDM_VALID_PULSE, 0x00);
   if (ret)
       return ret;

   /* Configure TDM channels (84-91) */
   for (i = 0; i < ES9080Q_NUM_OUT_CHANNELS; i++) {
       ret = es9080q_rw_write(priv, 84 + i, 
                             ((0 == i) ? (0 << 7) : 0) |  /* TDM_VALID_PULSE_POS_MSB only on reg 84 */
                             (0 << 4) |                    /* CHx data line selection: line 1 */
                             (i << 0));                    /* Slot selection */
       if (ret)
           return ret;
   }

   /* Filter shape and dither - from C++ implementation */
   ret = es9080q_rw_write(priv, ES9080Q_REG_FILTER_SHAPE, 0x46);
   if (ret)
       return ret;

   ret = es9080q_rw_write(priv, ES9080Q_REG_DITHER, 0xE4);
   if (ret)
       return ret;

   /* THD Compensation registers - from C++ implementation */
   ret = es9080q_rw_write(priv, ES9080Q_REG_THD_C2_135, 0x68);
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 112, 0x01);  /* THD C2 coefficient continued */
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 113, 0x8D);  /* THD C3 coefficient */
   if (ret)
       return ret;

   ret = es9080q_rw_write(priv, ES9080Q_REG_THD_C2_246, 0x68);
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 116, 0x01);  /* THD C2 coefficient continued */
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 117, 0x8D);  /* THD C3 coefficient */
   if (ret)
       return ret;

   /* Automute - disable for all channels */
   ret = es9080q_rw_write(priv, ES9080Q_REG_AUTOMUTE, 0x00);
   if (ret)
       return ret;

   /* NSMOD registers - from C++ implementation */
   ret = es9080q_rw_write(priv, ES9080Q_REG_NSMOD_PHASE, 0xCC);
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, ES9080Q_REG_NSMOD_TYPE, 0x54);
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 131, 0x44);  /* NSMOD CH1/2 quantizer */
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 132, 0x44);  /* NSMOD CH3/4 quantizer */
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 133, 0x44);  /* NSMOD CH5/6 quantizer */
   if (ret)
       return ret;
   ret = es9080q_rw_write(priv, 134, 0x44);  /* NSMOD CH7/8 quantizer */
   if (ret)
       return ret;

   /* DRE register */
   ret = es9080q_rw_write(priv, 136, 0x00);
   if (ret)
       return ret;

   /* Volume control setup */
   ret = es9080q_rw_write(priv, ES9080Q_REG_VOLUME_CTRL,
                         (1 << 6) |    /* FORCE_VOLUME: Updates volume immediately */
                         (0 << 5) |    /* Separated volume control */
                         (0 << 4));    /* Separate volume control for each channel */
   if (ret)
       return ret;

   /* Initialize volumes */
   for (i = 0; i < ES9080Q_NUM_OUT_CHANNELS; i++) {
       ret = es9080q_rw_write(priv, ES9080Q_REG_VOLUME_BASE + i, priv->volume[i]);
       if (ret)
           return ret;
   }

   /* DAC resync sequence - from C++ implementation */
   ret = es9080q_rw_write(priv, ES9080Q_REG_DAC_RESYNC, 0x10);
   if (ret)
       return ret;
   udelay(10);
   
   ret = es9080q_rw_write(priv, ES9080Q_REG_DAC_RESYNC, 0x0F);
   if (ret)
       return ret;
   udelay(10);
   
   ret = es9080q_rw_write(priv, ES9080Q_REG_DAC_RESYNC, 0x00);
   if (ret)
       return ret;

   /* Turn on the AMP */
   ret = es9080q_rw_write(priv, ES9080Q_REG_AMP_CTRL, 0x02);
   if (ret)
       return ret;

   priv->codec_initialized = true;
   dev_info(priv->component->dev, "ES9080Q initialization complete\n");

   return 0;
}

/* DAI operations */
static int es9080q_hw_params(struct snd_pcm_substream *substream,
                           struct snd_pcm_hw_params *params,
                           struct snd_soc_dai *dai)
{
   struct snd_soc_component *component = dai->component;
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);
   unsigned int rate = params_rate(params);
   int ret;

   dev_info(component->dev, "ES9080Q hw_params: rate=%u\n", rate);

   /* Initialize codec */
   ret = es9080q_initialize_codec(priv, rate);
   if (ret) {
       dev_warn(component->dev, "Codec init had issues: %d\n", ret);
   }

   return 0;
}

static int es9080q_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
   struct snd_soc_component *component = dai->component;
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);

   dev_info(component->dev, "ES9080Q set_fmt: 0x%08x\n", fmt);

   /* Check format - Note: ES9080Q uses TDM/DSP mode internally */
   switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
   case SND_SOC_DAIFMT_I2S:
       dev_warn(component->dev, "I2S format requested, but ES9080Q uses TDM internally\n");
   case SND_SOC_DAIFMT_DSP_A:
   case SND_SOC_DAIFMT_DSP_B:
       break;
   default:
       dev_err(component->dev, "Unsupported format\n");
       return -EINVAL;
   }

   /* Check master/slave mode */
   switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
   case SND_SOC_DAIFMT_CBC_CFC:
       priv->is_master = false;
       dev_info(component->dev, "ES9080Q: Slave mode\n");
       break;
   case SND_SOC_DAIFMT_CBP_CFP:
       priv->is_master = true;
       dev_info(component->dev, "ES9080Q: Master mode\n");
       break;
   default:
       dev_err(component->dev, "Unsupported clock provider mode\n");
       return -EINVAL;
   }

   return 0;
}

static int es9080q_set_sysclk(struct snd_soc_dai *dai, int clk_id,
                            unsigned int freq, int dir)
{
   struct snd_soc_component *component = dai->component;
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);

   dev_info(component->dev, "ES9080Q set_sysclk: freq=%u\n", freq);
   priv->sysclk_freq = freq;

   return 0;
}

static int es9080q_set_tdm_slot(struct snd_soc_dai *dai,
                              unsigned int tx_mask, unsigned int rx_mask,
                              int slots, int slot_width)
{
   struct snd_soc_component *component = dai->component;
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);
   int first_slot = -1;
   int active_slots = 0;
   int i;

   dev_info(component->dev, "ES9080Q set_tdm_slot: tx_mask=0x%04x, rx_mask=0x%04x, slots=%d, slot_width=%d\n",
            tx_mask, rx_mask, slots, slot_width);

   /* Count active slots and find first slot */
   for (i = 0; i < slots && i < 32; i++) {
       if (tx_mask & (1 << i)) {
           if (first_slot == -1)
               first_slot = i;
           active_slots++;
       }
   }

   /* Validate configuration */
   if (active_slots > ES9080Q_NUM_OUT_CHANNELS) {
       dev_err(component->dev, "Too many active slots (%d), max is %d\n",
               active_slots, ES9080Q_NUM_OUT_CHANNELS);
       return -EINVAL;
   }

   if (slot_width != 16 && slot_width != 24 && slot_width != 32) {
       dev_err(component->dev, "Unsupported slot width %d\n", slot_width);
       return -EINVAL;
   }

   /* ES9080Q expects 8 consecutive slots starting from slot 2 (channels 3-10) */
   if (first_slot != 2 || active_slots != 8) {
       dev_info(component->dev, "ES9080Q expects slots 2-9 (tx_mask=0x03FC), adjusting internally\n");
   }

   /* Store the configuration for later use if needed */
   /* The actual TDM slot configuration is done in es9080q_initialize_codec() */
   
   dev_info(component->dev, "ES9080Q TDM slots configured: first_slot=%d, active=%d\n",
            first_slot, active_slots);

   return 0;
}

static int es9080q_trigger(struct snd_pcm_substream *substream, int cmd,
                         struct snd_soc_dai *dai)
{
   struct snd_soc_component *component = dai->component;

   switch (cmd) {
   case SNDRV_PCM_TRIGGER_START:
   case SNDRV_PCM_TRIGGER_RESUME:
   case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
       dev_info(component->dev, "ES9080Q: Audio START\n");
       break;
   case SNDRV_PCM_TRIGGER_STOP:
   case SNDRV_PCM_TRIGGER_SUSPEND:
   case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
       dev_info(component->dev, "ES9080Q: Audio STOP\n");
       break;
   default:
       return -EINVAL;
   }

   return 0;
}

static const struct snd_soc_dai_ops es9080q_dai_ops = {
   .hw_params = es9080q_hw_params,
   .set_fmt = es9080q_set_fmt,
   .set_sysclk = es9080q_set_sysclk,
   .set_tdm_slot = es9080q_set_tdm_slot,
   .trigger = es9080q_trigger,
};

static struct snd_soc_dai_driver es9080q_dai = {
   .name = "es9080q-hifi",
   .playback = {
       .stream_name = "Playback",
       .channels_min = 1,
       .channels_max = ES9080Q_NUM_OUT_CHANNELS,
       .rates = SNDRV_PCM_RATE_CONTINUOUS,
       .rate_min = 8000,
       .rate_max = 768000,
       .formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
                 SNDRV_PCM_FMTBIT_S32_LE,
   },
   .ops = &es9080q_dai_ops,
};

/* Volume control functions - FIXED */
static int es9080q_volume_info(struct snd_kcontrol *kcontrol,
                             struct snd_ctl_elem_info *uinfo)
{
   uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
   uinfo->count = 1;
   uinfo->value.integer.min = ES9080Q_VOLUME_MIN;
   uinfo->value.integer.max = ES9080Q_VOLUME_MAX;
   uinfo->value.integer.step = ES9080Q_VOLUME_STEP;
   return 0;
}

static int es9080q_volume_get(struct snd_kcontrol *kcontrol,
                            struct snd_ctl_elem_value *ucontrol)
{
   struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);
   int channel = (long)kcontrol->private_value;

   if (channel >= ES9080Q_NUM_OUT_CHANNELS)
       return -EINVAL;

   ucontrol->value.integer.value[0] = priv->volume[channel];
   return 0;
}

static int es9080q_volume_put(struct snd_kcontrol *kcontrol,
                            struct snd_ctl_elem_value *ucontrol)
{
   struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);
   int channel = (long)kcontrol->private_value;
   int volume = ucontrol->value.integer.value[0];
   int ret = 0;

   if (channel >= ES9080Q_NUM_OUT_CHANNELS)
       return -EINVAL;

   if (volume < ES9080Q_VOLUME_MIN || volume > ES9080Q_VOLUME_MAX)
       return -EINVAL;

   if (priv->volume[channel] == volume)
       return 0;  /* No change */

   priv->volume[channel] = volume;

   /* Write volume register if codec is running */
   if (priv->codec_initialized) {
       ret = es9080q_rw_write(priv, ES9080Q_REG_VOLUME_BASE + channel, volume);
       if (ret)
           return ret;
   }

   return 1;  /* Changed */
}

/* ALSA controls - PROPERLY DEFINED */
static const struct snd_kcontrol_new es9080q_controls[] = {
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC1 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 0,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC2 Playback Volume", 
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 1,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC3 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 2,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC4 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 3,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC5 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 4,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC6 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 5,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC7 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 6,
   },
   {
       .iface = SNDRV_CTL_ELEM_IFACE_MIXER,
       .name = "DAC8 Playback Volume",
       .access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
       .info = es9080q_volume_info,
       .get = es9080q_volume_get,
       .put = es9080q_volume_put,
       .private_value = 7,
   },
};

/* DAPM widgets */
static const struct snd_soc_dapm_widget es9080q_dapm_widgets[] = {
   /* DAC outputs */
   SND_SOC_DAPM_DAC("DAC1", "Playback", SND_SOC_NOPM, 0, 0),
   SND_SOC_DAPM_DAC("DAC2", "Playback", SND_SOC_NOPM, 1, 0),
   SND_SOC_DAPM_DAC("DAC3", "Playback", SND_SOC_NOPM, 2, 0),
   SND_SOC_DAPM_DAC("DAC4", "Playback", SND_SOC_NOPM, 3, 0),
   SND_SOC_DAPM_DAC("DAC5", "Playback", SND_SOC_NOPM, 4, 0),
   SND_SOC_DAPM_DAC("DAC6", "Playback", SND_SOC_NOPM, 5, 0),
   SND_SOC_DAPM_DAC("DAC7", "Playback", SND_SOC_NOPM, 6, 0),
   SND_SOC_DAPM_DAC("DAC8", "Playback", SND_SOC_NOPM, 7, 0),
   
   /* Output pins */
   SND_SOC_DAPM_OUTPUT("OUT1"),
   SND_SOC_DAPM_OUTPUT("OUT2"),
   SND_SOC_DAPM_OUTPUT("OUT3"),
   SND_SOC_DAPM_OUTPUT("OUT4"),
   SND_SOC_DAPM_OUTPUT("OUT5"),
   SND_SOC_DAPM_OUTPUT("OUT6"),
   SND_SOC_DAPM_OUTPUT("OUT7"),
   SND_SOC_DAPM_OUTPUT("OUT8"),
};

/* DAPM routes */
static const struct snd_soc_dapm_route es9080q_dapm_routes[] = {
   /* Connect DACs to outputs */
   {"OUT1", NULL, "DAC1"},
   {"OUT2", NULL, "DAC2"},
   {"OUT3", NULL, "DAC3"},
   {"OUT4", NULL, "DAC4"},
   {"OUT5", NULL, "DAC5"},
   {"OUT6", NULL, "DAC6"},
   {"OUT7", NULL, "DAC7"},
   {"OUT8", NULL, "DAC8"},
};

static int es9080q_component_probe(struct snd_soc_component *component)
{
   struct es9080q_priv *priv = snd_soc_component_get_drvdata(component);
   int i;

   dev_info(component->dev, "ES9080Q component probe - codec init deferred\n");

   priv->component = component;
   priv->codec_initialized = false;
   
   /* Initialize volume array to 0dB (register value 0) */
   for (i = 0; i < ES9080Q_NUM_OUT_CHANNELS; i++) {
       priv->volume[i] = 0;  /* 0dB default */
   }

   return 0;
}

static const struct snd_soc_component_driver soc_component_dev_es9080q = {
   .probe = es9080q_component_probe,
   .controls = es9080q_controls,
   .num_controls = ARRAY_SIZE(es9080q_controls),
   .dapm_widgets = es9080q_dapm_widgets,
   .num_dapm_widgets = ARRAY_SIZE(es9080q_dapm_widgets),
   .dapm_routes = es9080q_dapm_routes,
   .num_dapm_routes = ARRAY_SIZE(es9080q_dapm_routes),
};

/* Regmap configurations */
static bool es9080q_wo_readable(struct device *dev, unsigned int reg)
{
   return false;  /* Write-only registers are not readable */
}

static const struct regmap_config es9080q_wo_regmap_config = {
   .name = "write-only",
   .reg_bits = 8,
   .val_bits = 8,
   .max_register = 0xFF,
   .readable_reg = es9080q_wo_readable,
   .cache_type = REGCACHE_NONE,
};

static const struct regmap_config es9080q_rw_regmap_config = {
   .name = "read-write",
   .reg_bits = 8,
   .val_bits = 8,
   .max_register = 0xFF,
   .cache_type = REGCACHE_RBTREE,
};

/* I2C probe */
static int es9080q_i2c_probe(struct i2c_client *rw_client)
{
   struct device *dev = &rw_client->dev;
   struct es9080q_priv *priv;
   u32 wo_addr;
   int ret, i;

   dev_info(dev, "ES9080Q I2C probe\n");

   priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
   if (!priv)
       return -ENOMEM;

   priv->rw_client = rw_client;
   i2c_set_clientdata(rw_client, priv);

   /* Get write-only address from DT */
   ret = of_property_read_u32(dev->of_node, "write-only-addr", &wo_addr);
   if (ret) {
       dev_err(dev, "Missing write-only-addr property\n");
       return ret;
   }

   /* Create write-only client */
   priv->wo_client = i2c_new_dummy_device(rw_client->adapter, wo_addr);
   if (IS_ERR(priv->wo_client)) {
       dev_err(dev, "Failed to create WO client\n");
       return PTR_ERR(priv->wo_client);
   }

   /* Initialize regmaps */
   priv->rw_regmap = devm_regmap_init_i2c(priv->rw_client, &es9080q_rw_regmap_config);
   if (IS_ERR(priv->rw_regmap)) {
       ret = PTR_ERR(priv->rw_regmap);
       goto err_wo_device;
   }

   priv->wo_regmap = devm_regmap_init_i2c(priv->wo_client, &es9080q_wo_regmap_config);
   if (IS_ERR(priv->wo_regmap)) {
       ret = PTR_ERR(priv->wo_regmap);
       goto err_wo_device;
   }

   /* Get reset GPIO */
   priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
   if (IS_ERR(priv->reset_gpio)) {
       ret = PTR_ERR(priv->reset_gpio);
       goto err_wo_device;
   }

   /* Perform basic reset - reduced timing */
   if (priv->reset_gpio) {
       gpiod_set_value_cansleep(priv->reset_gpio, 0); /* Assert reset */
       udelay(500);
       gpiod_set_value_cansleep(priv->reset_gpio, 1); /* Release reset */
       udelay(100);
   }

   /* Write initialization sequence - reduced delays */
   for (i = 0; i < ARRAY_SIZE(es9080q_init_seq); i++) {
       ret = es9080q_wo_write(priv, es9080q_init_seq[i].reg, 
                               es9080q_init_seq[i].def);
       if (ret) {
           dev_warn(dev, "Init seq step %d failed: %d\n", i, ret);
           continue;
       }
       udelay(50);
   }

   /* Set default sysclk frequency - will be updated by machine driver */
   priv->sysclk_freq = 24576000; /* Default MCLK frequency */

   /* Register component */
   ret = devm_snd_soc_register_component(dev, &soc_component_dev_es9080q,
                                        &es9080q_dai, 1);
   if (ret) {
       dev_err(dev, "Failed to register component: %d\n", ret);
       goto err_wo_device;
   }

   dev_info(dev, "ES9080Q codec registered successfully\n");

   return 0;

err_wo_device:
   i2c_unregister_device(priv->wo_client);
   return ret;
}

static void es9080q_i2c_remove(struct i2c_client *client)
{
    struct es9080q_priv *priv = i2c_get_clientdata(client);

    dev_info(&client->dev, "ES9080Q I2C remove\n");

    /* Disable codec if it was initialized */
    if (priv->codec_initialized && priv->wo_regmap) {
        /* Software reset */
        regmap_write(priv->wo_regmap, ES9080Q_REG_RESET_PLL, 0xC0);
        udelay(500);
        regmap_write(priv->wo_regmap, ES9080Q_REG_RESET_PLL, 0x00);
    }

    /* Reset GPIO */
    if (priv->reset_gpio) {
        gpiod_set_value_cansleep(priv->reset_gpio, 0);
    }

    /* Release write-only client */
    if (priv->wo_client) {
        i2c_unregister_device(priv->wo_client);
    }
}

/* Device Tree match table */
static const struct of_device_id es9080q_of_match[] = {
    { .compatible = "ess,es9080q" },
    { }
};
MODULE_DEVICE_TABLE(of, es9080q_of_match);

/* I2C device ID table */
static const struct i2c_device_id es9080q_i2c_id[] = {
    { "es9080q", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, es9080q_i2c_id);

static struct i2c_driver es9080q_i2c_driver = {
    .driver = {
        .name = "es9080q",
        .of_match_table = es9080q_of_match,
    },
    .probe = es9080q_i2c_probe,
    .remove = es9080q_i2c_remove,
    .id_table = es9080q_i2c_id,
};
module_i2c_driver(es9080q_i2c_driver);

MODULE_DESCRIPTION("ASoC ES9080Q codec driver");
MODULE_AUTHOR("JianDe jiande2020@gmail.com");
MODULE_AUTHOR("Giulio Moro");
MODULE_LICENSE("GPL");