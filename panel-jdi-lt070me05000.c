// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 InforceComputing
 * Author: Vinay Simha BN <simhavcs@gmail.com>
 *
 * Copyright (C) 2016 Linaro Ltd
 * Author: Sumit Semwal <sumit.semwal@linaro.org>
 *
 * From internet archives, the panel for Nexus 7 2nd Gen, 2013 model is a
 * JDI model LT070ME05000, and its data sheet is at:
 * http://panelone.net/en/7-0-inch/JDI_LT070ME05000_7.0_inch-datasheet
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

static const char * const regulator_names[] = {
	"vddp",
	"iovcc"
};

struct jdi_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;

	struct regulator_bulk_data supplies[ARRAY_SIZE(regulator_names)];

	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *dcdc_en_gpio;
	struct backlight_device *backlight;

	bool prepared;
	bool enabled;

	const struct drm_display_mode *mode;
};

static inline struct jdi_panel *to_jdi_panel(struct drm_panel *panel)
{
	return container_of(panel, struct jdi_panel, base);
}

static int jdi_panel_init(struct jdi_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &ctx->dsi->dev;
	
	dev_err(dev, "DSI init sequence start\n");

	mipi_dsi_dcs_write_seq(dsi,0xFE,0x01);
	mipi_dsi_dcs_write_seq(dsi,0x15,0x04);

	mipi_dsi_dcs_write_seq(dsi,0xFE,0x00);
	mipi_dsi_dcs_write_seq(dsi,0x35,0x00);
	mipi_dsi_dcs_write_seq(dsi,0x3A,0x75);
	mipi_dsi_dcs_write_seq(dsi,0x51,0xFF);

	mipi_dsi_dcs_write_seq(dsi,0x2A,0x00,0x0E,0x01,0xD3);
	mipi_dsi_dcs_write_seq(dsi,0x2B,0x00,0x00,0x01,0xC5);

	dev_err(dev, "DSI init sequence end\n");

	return 0;
}

static int jdi_panel_on(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	struct device *dev = &jdi->dsi->dev;
	int ret;

	dev_err(dev, "display on, start");

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	do {
		ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
		if (ret < 0) {
			dev_err(dev, "failed to exit sleep mode: %d\n", ret);
		}
	} while (ret == -ETIMEDOUT);

	msleep(120);

	do {
		ret = mipi_dsi_dcs_set_display_on(dsi);
		if (ret < 0)
			dev_err(dev, "failed to set display on: %d\n", ret);

		msleep(20);
	} while (ret == -ETIMEDOUT);

	dev_err(dev, "display on, end");

	return ret;
}

static void jdi_panel_off(struct jdi_panel *jdi)
{
	struct mipi_dsi_device *dsi = jdi->dsi;
	struct device *dev = &jdi->dsi->dev;
	int ret;

	dev_err(dev, "display off, start");

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		dev_err(dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		dev_err(dev, "failed to enter sleep mode: %d\n", ret);

	msleep(100);

	dev_err(dev, "display off, end");
}

static int jdi_panel_disable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct device *dev = &jdi->dsi->dev;

	dev_err(dev, "display disable, start");

	if (!jdi->enabled)
		return 0;

	backlight_disable(jdi->backlight);

	jdi->enabled = false;

	dev_err(dev, "display disable, end");

	return 0;
}

static int jdi_panel_unprepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct device *dev = &jdi->dsi->dev;
	int ret;

	dev_err(dev, "display unprepare, start");

	if (!jdi->prepared)
		return 0;

	jdi_panel_off(jdi);

	ret = regulator_bulk_disable(ARRAY_SIZE(jdi->supplies), jdi->supplies);
	if (ret < 0)
		dev_err(dev, "regulator disable failed, %d\n", ret);

	gpiod_set_value_cansleep(jdi->enable_gpio, 0);

	gpiod_set_value_cansleep(jdi->reset_gpio, 1);

	gpiod_set_value_cansleep(jdi->dcdc_en_gpio, 0);

	jdi->prepared = false;

	dev_err(dev, "display unprepare, end");

	return 0;
}

static int jdi_panel_prepare(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct device *dev = &jdi->dsi->dev;
	int ret;

	dev_err(dev, "Prepare start");

	if (jdi->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(jdi->supplies), jdi->supplies);
	if (ret < 0) {
		dev_err(dev, "regulator enable failed, %d\n", ret);
		return ret;
	}

	msleep(20);

	gpiod_set_value_cansleep(jdi->reset_gpio, 0);
	msleep(25);

	ret = jdi_panel_init(jdi);
	if (ret < 0) {
		dev_err(dev, "failed to init panel: %d\n", ret);
		goto poweroff;
	}

	ret = jdi_panel_on(jdi);
	if (ret < 0) {
		dev_err(dev, "failed to set panel on: %d\n", ret);
		goto poweroff;
	}

	jdi->prepared = true;

	dev_err(dev, "Prepare end");

	return 0;

poweroff:
	ret = regulator_bulk_disable(ARRAY_SIZE(jdi->supplies), jdi->supplies);
	if (ret < 0)
		dev_err(dev, "regulator disable failed, %d\n", ret);

	gpiod_set_value_cansleep(jdi->enable_gpio, 0);

	gpiod_set_value_cansleep(jdi->reset_gpio, 1);

	gpiod_set_value_cansleep(jdi->dcdc_en_gpio, 0);

	return ret;
}

static int jdi_panel_enable(struct drm_panel *panel)
{
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct device *dev = &jdi->dsi->dev;

	dev_err(dev, "display enable, start");

	if (jdi->enabled)
		return 0;

	msleep(200);

	jdi_panel_on(jdi);

	backlight_enable(jdi->backlight);

	jdi->enabled = true;

	dev_err(dev, "display enable, end");

	return 0;
}

static const struct drm_display_mode default_mode = {
		.clock = (454 + 60 + 20 + 60) * (454 + 10 + 8 + 12) * 60 / 1000, // 17249, //14374, //20124,

		.hdisplay = 454,
		.hsync_start = 454 + 60,
		.hsync_end = 454 + 60 + 20,
		.htotal = 454 + 60 + 20 + 60, // res, x , x , hsync

		.vdisplay = 454,
		.vsync_start = 454 + 10,
		.vsync_end = 454 + 10 + 8,
		.vtotal = 454 + 10 + 8 + 12, //(hactive + hfront_porch + hsync_len + hback_porch)

		.flags = 0, //DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
		// .type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,

		.width_mm = 35,
		.height_mm = 35,
};

static int jdi_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct jdi_panel *jdi = to_jdi_panel(panel);
	struct device *dev = &jdi->dsi->dev;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static int dsi_dcs_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int ret;
	u16 brightness = bl->props.brightness;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness & 0xff;
}

static int dsi_dcs_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, bl->props.brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static const struct backlight_ops dsi_bl_ops = {
	.update_status = dsi_dcs_bl_update_status,
	.get_brightness = dsi_dcs_bl_get_brightness,
};

static struct backlight_device *
drm_panel_create_dsi_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct backlight_properties props;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.brightness = 255;
	props.max_brightness = 255;

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &dsi_bl_ops, &props);
}

static const struct drm_panel_funcs jdi_panel_funcs = {
	.disable = jdi_panel_disable,
	.unprepare = jdi_panel_unprepare,
	.prepare = jdi_panel_prepare,
	.enable = jdi_panel_enable,
	.get_modes = jdi_panel_get_modes,
};

static const struct of_device_id jdi_of_match[] = {
	{ .compatible = "jdi,lt070me05000", },
	{ }
};
MODULE_DEVICE_TABLE(of, jdi_of_match);

static int jdi_panel_add(struct jdi_panel *jdi)
{
	struct device *dev = &jdi->dsi->dev;
	int ret;
	unsigned int i;

	jdi->mode = &default_mode;

	for (i = 0; i < ARRAY_SIZE(jdi->supplies); i++)
		jdi->supplies[i].supply = regulator_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(jdi->supplies),
				      jdi->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to init regulator, ret=%d\n", ret);
		return ret;
	}

	jdi->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->enable_gpio)) {
		ret = PTR_ERR(jdi->enable_gpio);
		dev_err(dev, "cannot get enable-gpio %d\n", ret);
		return ret;
	}

	jdi->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(jdi->reset_gpio)) {
		ret = PTR_ERR(jdi->reset_gpio);
		dev_err(dev, "cannot get reset-gpios %d\n", ret);
		return ret;
	}

	jdi->dcdc_en_gpio = devm_gpiod_get(dev, "dcdc-en", GPIOD_OUT_LOW);
	if (IS_ERR(jdi->dcdc_en_gpio)) {
		ret = PTR_ERR(jdi->dcdc_en_gpio);
		dev_err(dev, "cannot get dcdc-en-gpio %d\n", ret);
		return ret;
	}

	jdi->backlight = drm_panel_create_dsi_backlight(jdi->dsi);
	if (IS_ERR(jdi->backlight)) {
		ret = PTR_ERR(jdi->backlight);
		dev_err(dev, "failed to register backlight %d\n", ret);
		return ret;
	}

	//jdi->base.prepare_upstream_first = true;
	jdi->base.prepare_prev_first = true;
	drm_panel_init(&jdi->base, &jdi->dsi->dev, &jdi_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&jdi->base);

	return 0;
}

static void jdi_panel_del(struct jdi_panel *jdi)
{
	if (jdi->base.dev)
		drm_panel_remove(&jdi->base);
}

static int jdi_panel_probe(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi;
	int ret;

	dsi->lanes = 1;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags =  MIPI_DSI_MODE_VIDEO 
						| MIPI_DSI_MODE_LPM  
						| MIPI_DSI_MODE_VIDEO_BURST 
						| MIPI_DSI_CLOCK_NON_CONTINUOUS 
						| MIPI_DSI_MODE_VIDEO_HSE;

	jdi = devm_kzalloc(&dsi->dev, sizeof(*jdi), GFP_KERNEL);
	if (!jdi)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, jdi);

	jdi->dsi = dsi;

	ret = jdi_panel_add(jdi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		jdi_panel_del(jdi);
		return ret;
	}

	return 0;
}

static void jdi_panel_remove(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = jdi_panel_disable(&jdi->base);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to disable panel: %d\n", ret);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n",
			ret);

	jdi_panel_del(jdi);
}

static void jdi_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct jdi_panel *jdi = mipi_dsi_get_drvdata(dsi);

	jdi_panel_disable(&jdi->base);
}

static struct mipi_dsi_driver jdi_panel_driver = {
	.driver = {
		.name = "panel-jdi-lt070me05000",
		.of_match_table = jdi_of_match,
	},
	.probe = jdi_panel_probe,
	.remove = jdi_panel_remove,
	.shutdown = jdi_panel_shutdown,
};
module_mipi_dsi_driver(jdi_panel_driver);

MODULE_AUTHOR("Sumit Semwal <sumit.semwal@linaro.org>");
MODULE_AUTHOR("Vinay Simha BN <simhavcs@gmail.com>");
MODULE_DESCRIPTION("JDI LT070ME05000 WUXGA");
MODULE_LICENSE("GPL v2");
