// SPDX-License-Identifier: GPL-2.0-only
/*
 * Advantech embedded controller core driver AHC1EC0
 *
 * Copyright 2020 Advantech IIoT Group
 *
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/mfd/ahc1ec0.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define DRV_NAME      "ahc1ec0"

enum {
	ADVEC_SUBDEV_BRIGHTNESS = 0,
	ADVEC_SUBDEV_EEPROM,
	ADVEC_SUBDEV_GPIO,
	ADVEC_SUBDEV_HWMON,
	ADVEC_SUBDEV_LED,
	ADVEC_SUBDEV_WDT,
	ADVEC_SUBDEV_MAX,
};

/* Wait IBF (Input Buffer Full) clear */
static int ec_wait_write(void)
{
	int i;

	for (i = 0; i < EC_MAX_TIMEOUT_COUNT; i++) {
		if ((inb(EC_COMMAND_PORT) & EC_COMMAND_BIT_IBF) == 0)
			return 0;

		udelay(EC_RETRY_UDELAY);
	}

	return -ETIMEDOUT;
}

/* Wait OBF (Output Buffer Full) data ready */
static int ec_wait_read(void)
{
	int i;

	for (i = 0; i < EC_MAX_TIMEOUT_COUNT; i++) {
		if ((inb(EC_COMMAND_PORT) & EC_COMMAND_BIT_OBF) != 0)
			return 0;

		udelay(EC_RETRY_UDELAY);
	}

	return -ETIMEDOUT;
}

/* Read data from EC HW RAM, the process is the following:
 * Step 0. Wait IBF clear to send command
 * Step 1. Send read command to EC command port
 * Step 2. Wait IBF clear that means command is got by EC
 * Step 3. Send read address to EC data port
 * Step 4. Wait OBF data ready
 * Step 5. Get data from EC data port
 */
int read_hw_ram(struct adv_ec_platform_data *adv_ec_data, unsigned char addr, unsigned char *data)
{
	int ret;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_HW_RAM_READ, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(addr, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;
	*data = inb(EC_STATUS_PORT);

	mutex_unlock(&adv_ec_data->lock);

	return ret;

error:
	mutex_unlock(&adv_ec_data->lock);
	dev_err(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
	       __LINE__);

	return ret;
}

/* Write data to EC HW RAM
 * Step 0. Wait IBF clear to send command
 * Step 1. Send write command to EC command port
 * Step 2. Wait IBF clear that means command is got by EC
 * Step 3. Send write address to EC data port
 * Step 4. Wait IBF clear that means command is got by EC
 * Step 5. Send data to EC data port
 */
int write_hw_ram(struct adv_ec_platform_data *adv_ec_data, unsigned char addr, unsigned char data)
{
	int ret;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_HW_RAM_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(addr, EC_STATUS_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(data, EC_STATUS_PORT);

	mutex_unlock(&adv_ec_data->lock);

	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_err(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
	       __LINE__);

	return ret;
}
EXPORT_SYMBOL_GPL(write_hw_ram);

/* Get dynamic control table */
static int adv_get_dynamic_tab(struct adv_ec_platform_data *adv_ec_data)
{
	int i, ret;
	unsigned char pin_tmp, device_id;

	mutex_lock(&adv_ec_data->lock);

	for (i = 0; i < EC_MAX_TBL_NUM; i++) {
		adv_ec_data->dym_tbl[i].device_id = 0xff;
		adv_ec_data->dym_tbl[i].hw_pin_num = 0xff;
	}

	for (i = 0; i < EC_MAX_TBL_NUM; i++) {
		ret = ec_wait_write();
		if (ret) {
			dev_dbg(adv_ec_data->dev, "%s: ec_wait_write. line: %d\n", __func__,
				__LINE__);
			goto error;
		}
		outb(EC_TBL_WRITE_ITEM, EC_COMMAND_PORT);

		ret = ec_wait_write();
		if (ret) {
			dev_dbg(adv_ec_data->dev, "%s: ec_wait_write. line: %d\n", __func__,
				__LINE__);
			goto error;
		}
		outb(i, EC_STATUS_PORT);

		ret = ec_wait_read();
		if (ret) {
			dev_dbg(adv_ec_data->dev, "%s: ec_wait_read. line: %d\n", __func__,
				__LINE__);
			goto error;
		}

		/*
		 *  If item is defined, EC will return item number.
		 *  If table item is not defined, EC will return 0xFF.
		 */
		pin_tmp = inb(EC_STATUS_PORT);
		if (pin_tmp == 0xff) {
			dev_dbg(adv_ec_data->dev, "%s: inb(EC_STATUS_PORT)=0x%02x != 0xff.\n",
				__func__, pin_tmp);
			goto pass;
		}

		ret = ec_wait_write();
		if (ret) {
			dev_dbg(adv_ec_data->dev, "%s: ec_wait_write. line: %d\n", __func__,
				__LINE__);
			goto error;
		}
		outb(EC_TBL_GET_PIN, EC_COMMAND_PORT);

		ret = ec_wait_read();
		if (ret) {
			dev_dbg(adv_ec_data->dev, "%s: ec_wait_read. line: %d\n", __func__,
				__LINE__);
			goto error;
		}
		pin_tmp = inb(EC_STATUS_PORT) & 0xff;
		if (pin_tmp == 0xff) {
			dev_dbg(adv_ec_data->dev, "%s: pin_tmp(0x%02X). line: %d\n", __func__,
				pin_tmp, __LINE__);
			goto pass;
		}

		ret = ec_wait_write();
		if (ret)
			goto error;
		outb(EC_TBL_GET_DEVID, EC_COMMAND_PORT);

		ret = ec_wait_read();
		if (ret) {
			dev_dbg(adv_ec_data->dev, "%s: ec_wait_read. line: %d\n", __func__,
				__LINE__);
			goto error;
		}
		device_id = inb(EC_STATUS_PORT) & 0xff;

		dev_dbg(adv_ec_data->dev, "%s: device_id=0x%02X. line: %d\n", __func__,
			device_id, __LINE__);

		adv_ec_data->dym_tbl[i].device_id = device_id;
		adv_ec_data->dym_tbl[i].hw_pin_num = pin_tmp;
	}

pass:
	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);
	dev_err(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n",
		__func__, __LINE__);
	return ret;
}

int read_ad_value(struct adv_ec_platform_data *adv_ec_data, unsigned char hwpin,
		unsigned char multi)
{
	int ret;
	u32 ret_val;
	unsigned int LSB, MSB;

	mutex_lock(&adv_ec_data->lock);
	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_AD_INDEX_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(hwpin, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;

	if (inb(EC_STATUS_PORT) == 0xff) {
		mutex_unlock(&adv_ec_data->lock);
		return -1;
	}

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_AD_LSB_READ, EC_COMMAND_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;
	LSB = inb(EC_STATUS_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_AD_MSB_READ, EC_COMMAND_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;
	MSB = inb(EC_STATUS_PORT);
	ret_val = ((MSB << 8) | LSB) & 0x03FF;
	ret_val = ret_val * multi * 100;

	mutex_unlock(&adv_ec_data->lock);
	return ret_val;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
		__LINE__);

	return ret;
}
EXPORT_SYMBOL_GPL(read_ad_value);

int read_acpi_value(struct adv_ec_platform_data *adv_ec_data, unsigned char addr,
		unsigned char *pvalue)
{
	int ret;
	unsigned char value;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_ACPI_RAM_READ, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(addr, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;
	value = inb(EC_STATUS_PORT);
	*pvalue = value;

	mutex_unlock(&adv_ec_data->lock);

	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
		__LINE__);

	return ret;
}
EXPORT_SYMBOL_GPL(read_acpi_value);

int write_acpi_value(struct adv_ec_platform_data *adv_ec_data, unsigned char addr,
		unsigned char value)
{
	int ret;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_ACPI_DATA_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(addr, EC_STATUS_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(value, EC_STATUS_PORT);

	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
		__LINE__);

	return ret;
}

int read_gpio_status(struct adv_ec_platform_data *adv_ec_data, unsigned char PinNumber,
		unsigned char *pvalue)
{
	int ret;

	unsigned char gpio_status_value;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_INDEX_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(PinNumber, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;

	if (inb(EC_STATUS_PORT) == 0xff) {
		dev_err(adv_ec_data->dev, "%s: Read Pin Number error!!\n", __func__);
		mutex_unlock(&adv_ec_data->lock);
		return -1;
	}

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_STATUS_READ, EC_COMMAND_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;
	gpio_status_value = inb(EC_STATUS_PORT);

	*pvalue = gpio_status_value;
	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
		__LINE__);
	return ret;
}

int write_gpio_status(struct adv_ec_platform_data *adv_ec_data, unsigned char PinNumber,
		unsigned char value)
{
	int ret;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_INDEX_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(PinNumber, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;

	if (inb(EC_STATUS_PORT) == 0xff) {
		mutex_unlock(&adv_ec_data->lock);
		dev_err(adv_ec_data->dev, "%s: Read Pin Number error!!\n", __func__);
		return -1;
	}

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_STATUS_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(value, EC_STATUS_PORT);

	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);
	dev_err(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d", __func__,
		__LINE__);

	return ret;
}

int read_gpio_dir(struct adv_ec_platform_data *adv_ec_data, unsigned char PinNumber,
		unsigned char *pvalue)
{
	int ret;
	unsigned char gpio_dir_value;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_INDEX_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(PinNumber, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;

	if (inb(EC_STATUS_PORT) == 0xff) {
		mutex_unlock(&adv_ec_data->lock);
		dev_err(adv_ec_data->dev, "%s: Read Pin Number error!! line: %d\n", __func__,
			__LINE__);
		return -1;
	}

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_DIR_READ, EC_COMMAND_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;
	gpio_dir_value = inb(EC_STATUS_PORT);
	*pvalue = gpio_dir_value;

	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
			__LINE__);

	return ret;
}

int write_gpio_dir(struct adv_ec_platform_data *adv_ec_data, unsigned char PinNumber,
		unsigned char value)
{
	int ret;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_INDEX_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(PinNumber, EC_STATUS_PORT);

	ret = ec_wait_read();
	if (ret)
		goto error;

	if (inb(EC_STATUS_PORT) == 0xff) {
		mutex_unlock(&adv_ec_data->lock);
		dev_warn(adv_ec_data->dev, "%s: Read Pin Number error!! line: %d\n", __func__,
			__LINE__);

		return -1;
	}

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(EC_GPIO_DIR_WRITE, EC_COMMAND_PORT);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(value, EC_STATUS_PORT);

	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
			__LINE__);

	return ret;
}

int write_hwram_command(struct adv_ec_platform_data *adv_ec_data, unsigned char data)
{
	int ret;

	mutex_lock(&adv_ec_data->lock);

	ret = ec_wait_write();
	if (ret)
		goto error;
	outb(data, EC_COMMAND_PORT);

	mutex_unlock(&adv_ec_data->lock);
	return 0;

error:
	mutex_unlock(&adv_ec_data->lock);

	dev_warn(adv_ec_data->dev, "%s: Wait for IBF or OBF too long. line: %d\n", __func__,
			__LINE__);

	return ret;
}
EXPORT_SYMBOL_GPL(write_hwram_command);

static int adv_ec_get_productname(struct adv_ec_platform_data *adv_ec_data, char *product)
{
	const char *vendor, *device;
	int length = 0;

	/* Check it is Advantech board */
	vendor = dmi_get_system_info(DMI_SYS_VENDOR);
	if (memcmp(vendor, "Advantech", sizeof("Advantech")) != 0)
		return -ENODEV;

	/* Get product model name */
	device = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (device) {
		while ((device[length] != ' ')
			&& (length < AMI_ADVANTECH_BOARD_ID_LENGTH))
			length++;
		memset(product, 0, AMI_ADVANTECH_BOARD_ID_LENGTH);
		memmove(product, device, length);

		dev_info(adv_ec_data->dev, "BIOS Product Name = %s\n", product);

		return 0;
	}

	dev_warn(adv_ec_data->dev, "This device is not Advantech Board (%s)!\n", product);

	return -ENODEV;
}

static const struct mfd_cell adv_ec_sub_cells[] = {
	{ .name = "adv-ec-brightness", },
	{ .name = "adv-ec-eeprom", },
	{ .name = "adv-ec-gpio", },
	{ .name = "ahc1ec0-hwmon", },
	{ .name = "adv-ec-led", },
	{ .name = "ahc1ec0-wdt", },
};

static int adv_ec_init_ec_data(struct adv_ec_platform_data *adv_ec_data)
{
	int ret;

	adv_ec_data->sub_dev_mask = 0;
	adv_ec_data->sub_dev_nb = 0;
	adv_ec_data->dym_tbl = NULL;
	adv_ec_data->bios_product_name = NULL;

	mutex_init(&adv_ec_data->lock);

	/* Get product name */
	adv_ec_data->bios_product_name =
		devm_kzalloc(adv_ec_data->dev, AMI_ADVANTECH_BOARD_ID_LENGTH, GFP_KERNEL);
	if (!adv_ec_data->bios_product_name)
		return -ENOMEM;

	memset(adv_ec_data->bios_product_name, 0, AMI_ADVANTECH_BOARD_ID_LENGTH);
	ret = adv_ec_get_productname(adv_ec_data, adv_ec_data->bios_product_name);
	if (ret)
		return ret;

	/* Get pin table */
	adv_ec_data->dym_tbl = devm_kzalloc(adv_ec_data->dev,
					EC_MAX_TBL_NUM * sizeof(struct ec_dynamic_table),
					GFP_KERNEL);
	if (!adv_ec_data->dym_tbl)
		return -ENOMEM;

	ret = adv_get_dynamic_tab(adv_ec_data);

	return ret;
}

static int adv_ec_parse_prop(struct adv_ec_platform_data *adv_ec_data)
{
	int i, ret;
	u32 nb, sub_dev[ADVEC_SUBDEV_MAX];

	ret = device_property_read_u32(adv_ec_data->dev, "advantech,sub-dev-nb", &nb);
	if (ret < 0) {
		dev_err(adv_ec_data->dev, "get sub-dev-nb failed! (%d)\n", ret);
		return ret;
	}
	adv_ec_data->sub_dev_nb = nb;

	ret = device_property_read_u32_array(adv_ec_data->dev, "advantech,sub-dev",
					     sub_dev, nb);
	if (ret < 0) {
		dev_err(adv_ec_data->dev, "get sub-dev failed! (%d)\n", ret);
		return ret;
	}

	for (i = 0; i < nb; i++) {
		switch (sub_dev[i]) {
		case ADVEC_SUBDEV_BRIGHTNESS:
		case ADVEC_SUBDEV_EEPROM:
		case ADVEC_SUBDEV_GPIO:
		case ADVEC_SUBDEV_HWMON:
		case ADVEC_SUBDEV_LED:
		case ADVEC_SUBDEV_WDT:
			adv_ec_data->sub_dev_mask |= BIT(sub_dev[i]);
			break;
		default:
			dev_err(adv_ec_data->dev, "invalid prop value(%d)!\n",
				sub_dev[i]);
		}
	}
	dev_info(adv_ec_data->dev, "sub-dev mask = 0x%x\n", adv_ec_data->sub_dev_mask);

	return 0;
}

static int adv_ec_probe(struct platform_device *pdev)
{
	int ret, i;
	struct device *dev = &pdev->dev;
	struct adv_ec_platform_data *adv_ec_data;

	adv_ec_data = devm_kzalloc(dev, sizeof(struct adv_ec_platform_data), GFP_KERNEL);
	if (!adv_ec_data)
		return -ENOMEM;

	dev_set_drvdata(dev, adv_ec_data);
	adv_ec_data->dev = dev;

	ret = adv_ec_init_ec_data(adv_ec_data);
	if (ret)
		goto err_init_data;

	ret = adv_ec_parse_prop(adv_ec_data);
	if (ret)
		goto err_prop;

	/* check whether this EC has the following subdevices. */
	for (i = 0; i < ARRAY_SIZE(adv_ec_sub_cells); i++) {
		if (adv_ec_data->sub_dev_mask & BIT(i)) {
			ret = mfd_add_hotplug_devices(dev, &adv_ec_sub_cells[i], 1);
			dev_info(adv_ec_data->dev, "mfd_add_hotplug_devices[%d] %s\n", i,
				adv_ec_sub_cells[i].name);
			if (ret)
				dev_err(dev, "failed to add %s subdevice: %d\n",
					adv_ec_sub_cells[i].name, ret);
		}
	}

	dev_info(adv_ec_data->dev, "Advantech EC probe done");

	return 0;

err_prop:
	dev_err(dev, "failed to probe\n");

err_init_data:
	mutex_destroy(&adv_ec_data->lock);

	dev_err(dev, "failed to init data\n");

	return ret;
}

static int adv_ec_remove(struct platform_device *pdev)
{
	struct adv_ec_platform_data *adv_ec_data;

	adv_ec_data = dev_get_drvdata(&pdev->dev);

	mutex_destroy(&adv_ec_data->lock);

	mfd_remove_devices(&pdev->dev);

	return 0;
}

static const struct of_device_id adv_ec_of_match[] __maybe_unused = {
	{
		.compatible = "advantech,ahc1ec0",
	},
	{}
};
MODULE_DEVICE_TABLE(of, adv_ec_of_match);

static const struct acpi_device_id adv_ec_acpi_match[] __maybe_unused = {
	{"AHC1EC0", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, adv_ec_acpi_match);

static struct platform_driver adv_ec_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = of_match_ptr(adv_ec_of_match),
		.acpi_match_table = ACPI_PTR(adv_ec_acpi_match),
	},
	.probe = adv_ec_probe,
	.remove = adv_ec_remove,
};
module_platform_driver(adv_ec_driver);

MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_DESCRIPTION("Advantech Embedded Controller core driver.");
MODULE_AUTHOR("Campion Kang <campion.kang@advantech.com.tw>");
MODULE_AUTHOR("Jianfeng Dai <jianfeng.dai@advantech.com.cn>");
MODULE_VERSION("1.0");
