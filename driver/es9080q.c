// SPDX-License-Identifier: GPL-2.0-only
/*
 * es9080q.c  --  ES9080Q ALSA SoC Audio driver
 *
 * Copyright 2025 BeagleBoard.org
 *
 * Author: Jian De <jiande2020@gmail.com>,
 *         Giulio Moro
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include "es9080q.h"

static const struct {
    u8 reg;
    u8 val;
} es9080q_init_seq[] = {
    /* Set GPIO1 (MCLK) pad to input mode, invert CLKHV phase for better DNR */
    { 192, 0x03 },
    /* PLL bypass, remove 10k DVDD shunt, set PLL input to MCLK, enable PLL inputs */
    { 193, 0xC3 },
    /* PLL parameters */
    { 202, 0x40 },
};

struct codec_data {
    struct i2c_client *rw_client;    // Read/write client (0x48)
    struct i2c_client *wo_client;    // Write-only client (0x4c)
    struct regmap *rw_regmap;
    struct regmap *wo_regmap;
	struct gpio_desc *reset_gpio;  // GPIO-based reset
};

static bool es9080_wo_readable(struct device *dev, unsigned int reg)
{
    return false;  // All registers unreadable
}

/* Write-only regmap configuration */
static const struct regmap_config codec_wo_regmap_config = {
    .name = "write-only",
    .reg_bits = 8,
    .val_bits = 8,
    .max_register = 0xFF,
    .readable_reg = es9080_wo_readable,
    .cache_type = REGCACHE_NONE,   // No caching
};

/* Read/write regmap configuration */
static const struct regmap_config codec_rw_regmap_config = {
    .name = "read-write",
    .reg_bits = 8,
    .val_bits = 8,
    .max_register = 0xFF,
    .cache_type = REGCACHE_RBTREE,
};

static int es9080_i2c_probe(struct i2c_client *rw_client)
{
    struct device *dev = &rw_client->dev;
    struct codec_data *priv;
    u32 wo_addr;
    int i, ret;

    /* Allocate private data */
    priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
	
    /* Store read/write client */
    priv->rw_client = rw_client;
    i2c_set_clientdata(rw_client, priv); // Set client data early

    dev_info(dev, "Probing ES9080Q at R/W address 0x%02X\n", 
             rw_client->addr);

    /* Get write-only address from DT */
    ret = of_property_read_u32(dev->of_node, "write-only-addr", &wo_addr);
    if (ret) {
        dev_err(dev, "Missing write-only-addr property\n");
        return ret;
    }

   /* Create write-only client */
    priv->wo_client = i2c_new_dummy_device(rw_client->adapter, wo_addr);
    if (IS_ERR(priv->wo_client)) {
        dev_err(dev, "Failed to create WO client at 0x%02X\n", wo_addr);
        return PTR_ERR(priv->wo_client);
    }
    dev_info(dev, "Created WO client at 0x%02X\n", wo_addr);

    /* Initialize regmaps */
    priv->rw_regmap = devm_regmap_init_i2c(priv->rw_client, &codec_rw_regmap_config);
    if (IS_ERR(priv->rw_regmap)) {
        ret = PTR_ERR(priv->rw_regmap);
        dev_err(dev, "Failed to init R/W regmap: %d\n", ret);
        goto err_wo_device;
    }

    priv->wo_regmap = devm_regmap_init_i2c(priv->wo_client, &codec_wo_regmap_config);
    if (IS_ERR(priv->wo_regmap)) {
        ret = PTR_ERR(priv->wo_regmap);
        dev_err(dev, "Failed to init WO regmap: %d\n", ret);
        goto err_wo_device;
    }

    /* Initialize hardware */
    /* Parse optional reset (reset-gpios) */
    priv->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(priv->reset_gpio)) {
        ret = PTR_ERR(priv->reset_gpio);
        dev_err_probe(dev, ret, "Failed to get reset GPIO\n");
        goto err_wo_device;
    }

    /* Ensure reset is deasserted (low) before initial writes */
    if (priv->reset_gpio) {
        gpiod_set_value_cansleep(priv->reset_gpio, 1);  // Deassert reset (high)
        dev_info(dev, "Deasserted reset before initial writes\n");
    }

    /* Initial writes before reset (as per power-up sequence) */
    dev_info(dev, "Performing initial writes before reset\n");
    for (int write_idx = 0; write_idx < 4; write_idx++) {
        ret = regmap_write(priv->wo_regmap, 0xC0, 0x00);
        if (ret) {
            dev_warn(dev, "Initial write failed (attempt %d): %d\n", write_idx + 1, ret);
            
            /* Add delay between retries */
            if (write_idx < 3) {
                udelay(100);  // Short delay before retry
            }
        } else {
            dev_dbg(dev, "Initial write successful (attempt %d)\n", write_idx + 1);
            break;  // Success, break out of retry loop
        }
        
        /* If all 4 attempts failed */
        if (write_idx == 3 && ret) {
            dev_err(dev, "All initial writes failed: %d\n", ret);
            return ret;
        }
    }

    /* Hardware reset sequence */
    if (priv->reset_gpio) {
        /* Assert reset (active low) */
        gpiod_set_value_cansleep(priv->reset_gpio, 0); 
        /* Minimum 3ms reset pulse */
        usleep_range(1000, 2000);
        /* Release reset */
        gpiod_set_value_cansleep(priv->reset_gpio, 1);
        /* Power-on reset time */
        udelay(100);

        dev_info(dev, "Performed hardware reset\n");
    } else {
        dev_info(dev, "No reset GPIO specified\n");
    }
	
	/* Write initialization sequence */
    for (i = 0; i < ARRAY_SIZE(es9080q_init_seq); i++) {
        ret = regmap_write(priv->wo_regmap,
                           es9080q_init_seq[i].reg,
                           es9080q_init_seq[i].val);
        if (ret) {
            dev_err(&rw_client->dev,
                    "Reg write failed reg=0x%02x: %d\n",
                    es9080q_init_seq[i].reg, ret);
            return ret;
        }
    }
    
    udelay(100);  // Short delay before retry

    /* Verify communication */
    unsigned int status;
    ret = regmap_write(priv->rw_regmap, CODEC_REG_STATUS, 0xFF);
    if (ret) {
        dev_err(dev, "Status write failed: %d\n", ret);
        return ret;
    }
    
    dev_info(dev, "Codec initialized. Status: 0x%02X\n", status);

    return 0;

err_wo_device:
    i2c_unregister_device(priv->wo_client);
    return ret;
}

static void es9080_i2c_remove(struct i2c_client *client)
{
    struct codec_data *priv = i2c_get_clientdata(client);
    
    /* Release write-only client */
    i2c_unregister_device(priv->wo_client);
}

/* Device Tree match table */
static const struct of_device_id es9080_of_match[] = {
    { .compatible = "ess,es9080q" },
    {}
};
MODULE_DEVICE_TABLE(of, es9080_of_match);

/* I2C driver structure */
static struct i2c_driver es9080_i2c_driver = {
    .driver = {
        .name = "es9080",
        .of_match_table = es9080_of_match,
    },
    .probe = es9080_i2c_probe,
    .remove = es9080_i2c_remove,
};
module_i2c_driver(es9080_i2c_driver);

MODULE_DESCRIPTION("ASoC ES9080Q driver");
MODULE_AUTHOR("JianDe <jiande2020@gmail.com>");
MODULE_LICENSE("GPL");