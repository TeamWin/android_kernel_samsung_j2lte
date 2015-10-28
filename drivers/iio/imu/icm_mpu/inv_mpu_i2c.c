/*
* Copyright (C) 2012 Invensense, Inc.
*
* This software is licensed under the terms of the GNU General Public
* License version 2, as published by the Free Software Foundation, and
* may be copied, distributed, and modified under those terms.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#define pr_fmt(fmt) "inv_mpu: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/jiffies.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>

#include "inv_mpu_iio.h"

#define CONFIG_DYNAMIC_DEBUG_I2C 0

#ifdef CONFIG_DTS_INV_MPU_IIO
#include "inv_mpu_dts.h"
#endif

/**
 *  inv_i2c_read_base() - Read one or more bytes from the device registers.
 *  @st:	Device driver instance.
 *  @i2c_addr:  i2c address of device.
 *  @reg:	First device register to be read from.
 *  @length:	Number of bytes to read.
 *  @data:	Data read from device.
 *  NOTE:This is not re-implementation of i2c_smbus_read because i2c
 *       address could be specified in this case. We could have two different
 *       i2c address due to secondary i2c interface.
 */
int inv_i2c_read_base(struct inv_mpu_state *st, u16 i2c_addr,
	u8 reg, u16 length, u8 *data)
{
	struct i2c_msg msgs[2];
	int res;

	if (!data)
		return -EINVAL;

	msgs[0].addr = i2c_addr;
	msgs[0].flags = 0;	/* write */
	msgs[0].buf = &reg;
	msgs[0].len = 1;

	msgs[1].addr = i2c_addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].buf = data;
	msgs[1].len = length;

	res = i2c_transfer(st->sl_handle, msgs, 2);

	if (res < 2) {
		if (res >= 0)
			res = -EIO;
	} else
		res = 0;
//	INV_I2C_INC_MPUWRITE(3);
//	INV_I2C_INC_MPUREAD(length);

	return res;
}

/**
 *  inv_i2c_single_write_base() - Write a byte to a device register.
 *  @st:	Device driver instance.
 *  @i2c_addr:  I2C address of the device.
 *  @reg:	Device register to be written to.
 *  @data:	Byte to write to device.
 *  NOTE:This is not re-implementation of i2c_smbus_write because i2c
 *       address could be specified in this case. We could have two different
 *       i2c address due to secondary i2c interface.
 */
int inv_i2c_single_write_base(struct inv_mpu_state *st,
	u16 i2c_addr, u8 reg, u8 data)
{
	u8 tmp[2];
	struct i2c_msg msg;
	int res;

	tmp[0] = reg;
	tmp[1] = data;

	msg.addr = i2c_addr;
	msg.flags = 0;	/* write */
	msg.buf = tmp;
	msg.len = 2;

//	INV_I2C_INC_MPUWRITE(3);

	res = i2c_transfer(st->sl_handle, &msg, 1);
	if (res < 1) {
		if (res == 0)
			res = -EIO;
		return res;
	} else
		return 0;
}

int inv_plat_single_write(struct inv_mpu_state *st, u8 reg, u8 data)
{
	return inv_i2c_single_write_base(st, st->i2c_addr, reg, data);
}

int inv_plat_read(struct inv_mpu_state *st, u8 reg, int len, u8 *data)
{
	return inv_i2c_read_base(st, st->i2c_addr, reg, len, data);
}

static int _memory_write(struct inv_mpu_state *st, u8 mpu_addr, u16 mem_addr,
		     u32 len, u8 const *data)
{
	u8 bank[2];
	u8 addr[2];
	u8 buf[513];

	struct i2c_msg msgs[3];
	int res;

	if (!data || !st)
		return -EINVAL;

	if (len >= (sizeof(buf) - 1))
		return -ENOMEM;

	bank[0] = REG_MEM_BANK_SEL;
	bank[1] = mem_addr >> 8;

	addr[0] = REG_MEM_START_ADDR;
	addr[1] = mem_addr & 0xFF;

	buf[0] = REG_MEM_R_W;
	memcpy(buf + 1, data, len);

	/* write message */
	msgs[0].addr = mpu_addr;
	msgs[0].flags = 0;
	msgs[0].buf = bank;
	msgs[0].len = sizeof(bank);

	msgs[1].addr = mpu_addr;
	msgs[1].flags = 0;
	msgs[1].buf = addr;
	msgs[1].len = sizeof(addr);

	msgs[2].addr = mpu_addr;
	msgs[2].flags = 0;
	msgs[2].buf = (u8 *)buf;
	msgs[2].len = len + 1;

//	INV_I2C_INC_MPUWRITE(3 + 3 + (2 + len));

#if CONFIG_DYNAMIC_DEBUG_I2C
	{
		char *write = 0;
		pr_debug("%s WM%02X%02X%02X%s%s - %d\n", st->hw->name,
			 mpu_addr, bank[1], addr[1],
			 wr_pr_debug_begin(data, len, write),
			 wr_pr_debug_end(write),
			 len);
	}
#endif

	res = i2c_transfer(st->sl_handle, msgs, 3);
	if (res != 3) {
		if (res >= 0)
			res = -EIO;
		return res;
	} else {
		return 0;
	}
}

int mpu_memory_write(struct inv_mpu_state *st, u8 mpu_addr, u16 mem_addr,
		     u32 len, u8 const *data)
{
	int r, i, j;
#define DMP_MEM_CMP_SIZE 16
	u8 w[DMP_MEM_CMP_SIZE];
	bool retry;

	j = 0;
	retry = true;
	while ((j < 3) && retry) {
		retry = false;
		r = _memory_write(st, mpu_addr, mem_addr, len, data);
		if (len < DMP_MEM_CMP_SIZE) {
			r = mem_r(mem_addr, len, w);
			for (i = 0; i < len; i++) {
				if (data[i] != w[i]) {
					INVLOG(ERR, "error write=%x, len=%d,data=%x, w=%x\n",
						mem_addr, len, data[i], w[i]);
					retry = true;
				}
			}
		}
		j++;
	}

	return r;
}

int mpu_memory_read(struct inv_mpu_state *st, u8 mpu_addr, u16 mem_addr,
		    u32 len, u8 *data)
{
	u8 bank[2];
	u8 addr[2];
	u8 buf;

	struct i2c_msg msgs[4];
	int res;

	if (!data || !st)
		return -EINVAL;

	bank[0] = REG_MEM_BANK_SEL;
	bank[1] = mem_addr >> 8;

	addr[0] = REG_MEM_START_ADDR;
	addr[1] = mem_addr & 0xFF;

	buf = REG_MEM_R_W;

	/* write message */
	msgs[0].addr = mpu_addr;
	msgs[0].flags = 0;
	msgs[0].buf = bank;
	msgs[0].len = sizeof(bank);

	msgs[1].addr = mpu_addr;
	msgs[1].flags = 0;
	msgs[1].buf = addr;
	msgs[1].len = sizeof(addr);

	msgs[2].addr = mpu_addr;
	msgs[2].flags = 0;
	msgs[2].buf = &buf;
	msgs[2].len = 1;

	msgs[3].addr = mpu_addr;
	msgs[3].flags = I2C_M_RD;
	msgs[3].buf = data;
	msgs[3].len = len;

	res = i2c_transfer(st->sl_handle, msgs, 4);
	if (res != 4) {
		if (res >= 0)
			res = -EIO;
	} else
		res = 0;
//        INV_I2C_INC_MPUWRITE(3 + 3 + 3);
//        INV_I2C_INC_MPUREAD(len);
#if CONFIG_DYNAMIC_DEBUG_I2C
	{
		char *read = 0;
		pr_debug("%s RM%02X%02X%02X%02X - %s%s\n", st->hw->name,
			 mpu_addr, bank[1], addr[1], len,
			 wr_pr_debug_begin(data, len, read),
			 wr_pr_debug_end(read));
	}
#endif

	return res;
}

#if defined(CONFIG_SENSORS)
static ssize_t inv_reactive_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	st = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", st->reactive_state);
}

static ssize_t inv_reactive_store(struct device *dev,
				struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct inv_mpu_state *st = dev_get_drvdata(dev);
	bool onoff = false;
	unsigned long enable = 0;
	int result;

	if (strict_strtoul(buf, 10, &enable)) {
		pr_err("[SENSOR] %s, kstrtoint fail\n", __func__);
		return -EINVAL;
	}

	st->reactive_factory = 0;
	if (enable == 0) {
		onoff = false;
	} else if (enable == 1) {
		onoff = true;
	} else if (enable == 2) {
		onoff = true;
		st->reactive_factory = 1;
	} else {
		pr_err("[SENSOR] %s: invalid value %lu\n", __func__, enable);
		return -EINVAL;
	}

	if(onoff) {
		st->reactive_time = get_time_ns();
	}
	else {
		st->reactive_state = 0;
		if (st->reactive_factory) st->reactive_factory = false;
	}

	result = inv_switch_power_in_lp(st, true);

	st->reactive_enable = enable;

	result = inv_wom_enable(st, onoff);
	
	result = inv_switch_power_in_lp(st, false);

	pr_info("[SENSOR] %s: onoff = %d, state =%d OUT\n", __func__,
			st->reactive_enable,
			st->reactive_state);

	return size;
}

static ssize_t inv_mpu_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", VENDOR_NAME);
}

static ssize_t inv_mpu_name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", MODEL_NAME);
}

static ssize_t inv_accel_raw_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	signed short x, y, z;
	signed short cx, cy, cz;
	signed short m[9];

	st = dev_get_drvdata(dev);

	x = st->accel_data[0] + st->cal_data[0];
	y = st->accel_data[1] + st->cal_data[1];
	z = st->accel_data[2] + st->cal_data[2];

	cx = cy = cz = 0;

	if(st->plat_data.orientation != NULL)
	{
		int i;
		for(i=0; i<9;i++)
			m[i] = st->plat_data.orientation[i];

		cx = m[0]*x + m[1]*y + m[2]*z;
		cy = m[3]*x + m[4]*y + m[5]*z;
		cz = m[6]*x + m[7]*y + m[8]*z;
	}

	return snprintf(buf, PAGE_SIZE, "%d, %d, %d\n", cx, cy, cz);
}

static ssize_t inv_mpu_acc_selftest_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct inv_mpu_state *st;
	struct iio_dev *indio_dev;
	int accel_ratio[3] = {0,};
	int result = 0;

	st = dev_get_drvdata(dev);
	indio_dev = iio_priv_to_dev(st);

	mutex_lock(&indio_dev->mlock);
	inv_switch_power_in_lp(st, true);

	result = inv_accel_self_test(st, accel_ratio);

	inv_check_sensor_on(st);
	set_inv_enable(indio_dev);
	inv_switch_power_in_lp(st, false);
	mutex_unlock(&indio_dev->mlock);

	if(result == 0)
		pr_info("%s : selftest success. ret:%d\n", __func__, result);
	else if(result == 1)
		pr_info("%s : selftest(accel) failed. ret:%d\n", __func__, result);

	pr_info("%s : %d.%01d,%d.%01d,%d.%01d\n", __func__,
		(int)abs(accel_ratio[0]/10),
		(int)abs(accel_ratio[0])%10,
		(int)abs(accel_ratio[1]/10),
		(int)abs(accel_ratio[1])%10,
		(int)abs(accel_ratio[2]/10),
		(int)abs(accel_ratio[2])%10);

	return sprintf(buf, "%d,"
		"%d.%01d,%d.%01d,%d.%01d\n",
		result,
		(int)abs(accel_ratio[0]/10),
		(int)abs(accel_ratio[0])%10,
		(int)abs(accel_ratio[1]/10),
		(int)abs(accel_ratio[1])%10,
		(int)abs(accel_ratio[2]/10),
		(int)abs(accel_ratio[2])%10);
}

static int accel_do_calibrate(struct inv_mpu_state *st, int enable)
{
	int result, i;
	struct file *cal_filp;
	int sum[3] = { 0, };
	mm_segment_t old_fs = {0};

	if (enable) {
		struct iio_dev *indio_dev;
		signed short x, y, z;
		unsigned char data[6];		
		bool acc_enable;
		int rate;

		indio_dev = iio_priv_to_dev(st);


		rate = st->sensor_l[SENSOR_L_ACCEL].rate;
		acc_enable = st->sensor_l[SENSOR_L_ACCEL].on;

		if (!acc_enable)
			st->sensor_l[SENSOR_L_ACCEL].on = true;
		st->sensor_l[SENSOR_L_ACCEL].rate = 55;

		inv_check_sensor_rate(st);
		inv_check_sensor_on(st);
		set_inv_enable(indio_dev);


		for (i = 0; i < 10; i++) {
			result = inv_plat_read(st, 0x2D, BYTES_PER_SENSOR, data);
			if (result) {
				pr_err("%s,Could not accel enable fail.\n", __func__);
				return result;
			}

			x = (signed short)((data[0] << 8) | data[1]);
			y = (signed short)((data[2] << 8) | data[3]);
			z = (signed short)((data[4] << 8) | data[5]);

			sum[0] += 0 - x;
			sum[1] += 0 - y;
			if (z > 0)
				sum[2] += 8192 - z;
			else
				sum[2] += -8192 - z;
			usleep_range(20000, 21000);
		}

		for (i = 0; i < 3 ; i++)
			st->cal_data[i] = sum[i] / 10;
		

		st->sensor_l[SENSOR_L_ACCEL].rate = rate;
		if (!acc_enable)
			st->sensor_l[SENSOR_L_ACCEL].on = false;

		inv_check_sensor_rate(st);
		inv_check_sensor_on(st);
		set_inv_enable(indio_dev);
	} else {
		for (i = 0; i < 3 ; i++) {
			sum[i] = st->cal_data[i];
			st->cal_data[i] = 0;
		}
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(FACTORY_ACCEL_CAL_PATH,
			O_CREAT | O_TRUNC | O_WRONLY,
			S_IRUGO | S_IWUSR | S_IWGRP);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		result = PTR_ERR(cal_filp);
		goto done;
	}

	result = cal_filp->f_op->write(cal_filp,
		(char *)&st->cal_data, 3 * sizeof(int),
			&cal_filp->f_pos);
	if (result != 3 * sizeof(int)) {
		pr_err("%s: Can't write the cal data to file\n", __func__);
		if (enable)
			for (i = 0; i < 3 ; i++)
				st->cal_data[i] = 0;
		else
			for (i = 0; i < 3 ; i++)
				st->cal_data[i] = sum[i];

		result = -EIO;
	}

	filp_close(cal_filp, current->files);
	result = 0;
done:
	set_fs(old_fs);

	return result;
}

static int accel_open_calibration(struct inv_mpu_state *st)
{
	struct file *cal_filp = NULL;
	int err = 0;
	mm_segment_t old_fs = {0};

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cal_filp = filp_open(FACTORY_ACCEL_CAL_PATH,
		O_RDONLY, S_IRUGO | S_IWUSR | S_IWGRP);
	if (IS_ERR(cal_filp)) {
		pr_err("%s: Can't open calibration file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cal_filp);
		goto done;
	}

	err = cal_filp->f_op->read(cal_filp,
		(char *)&st->cal_data,
			3 * sizeof(int), &cal_filp->f_pos);
	if (err != 3 * sizeof(int)) {
		pr_err("%s: Can't read the cal data from file\n", __func__);
		err = -EIO;
	}

	pr_info("%s: (%d,%d,%d)\n", __func__,
		st->cal_data[0], st->cal_data[1],
			st->cal_data[2]);

	filp_close(cal_filp, current->files);
done:
	set_fs(old_fs);
	return err;
}

static ssize_t inv_accel_cal_store(struct device *dev,
				struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct inv_mpu_state *st;
	int err, enable;
	err = kstrtoint(buf, 10, &enable);
	pr_info("%s\n", __func__);
	if (err) {
		pr_err("%s, kstrtoint fail\n", __func__);
	} else {
		st = dev_get_drvdata(dev);
		inv_switch_power_in_lp(st, true);
		err = accel_do_calibrate(st, enable);
		inv_switch_power_in_lp(st, false);
		if (err) {
			pr_err("%s, accel calibration fail\n", __func__);
		}
	}

	return size;
}

static ssize_t inv_accel_cal_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_mpu_state *st;
	int err;
	st = dev_get_drvdata(dev);

	err = accel_open_calibration(st);
	if (!st->cal_data[0] && !st->cal_data[1] && !st->cal_data[2])
		err = -1;
	else
		err = 1;
	return snprintf(buf, PAGE_SIZE, "%d, %d, %d, %d\n", err,
		st->cal_data[0], st->cal_data[1], st->cal_data[2]);
}

static ssize_t inv_lowpassfilter_store(struct device *dev,
				struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct inv_mpu_state *st;
	int err, enable;
	err = kstrtoint(buf, 10, &enable);

	if (err) {
		pr_err("%s, kstrtoint fail\n", __func__);
		return -EINVAL;
	}

	st = dev_get_drvdata(dev);
	INVLOG(IL2, "%s, %d\n", __func__, enable);

	inv_lpf_enable(st, enable);

	return size;
}

static struct device_attribute dev_attr_acc_vendor =
	__ATTR(vendor, S_IRUSR | S_IRGRP,
	inv_mpu_vendor_show, NULL);
static struct device_attribute dev_attr_acc_name =
	__ATTR(name, S_IRUSR | S_IRGRP,
	inv_mpu_name_show, NULL);
static struct device_attribute dev_attr_acc_raw_data =
	__ATTR(raw_data, S_IRUSR | S_IRGRP,
	inv_accel_raw_data_show, NULL);

static struct device_attribute dev_attr_acc_calibration =
	__ATTR(calibration, S_IRUGO | S_IWUSR | S_IWGRP,
		inv_accel_cal_show, inv_accel_cal_store);
static struct device_attribute dev_attr_acc_reactive_alert =
	__ATTR(reactive_alert, S_IRUGO | S_IWUSR | S_IWGRP,
		inv_reactive_show, inv_reactive_store);
static struct device_attribute dev_attr_acc_selftest =
	__ATTR(selftest, S_IRUSR | S_IRGRP,
	inv_mpu_acc_selftest_show, NULL);
static struct device_attribute dev_attr_acc_lowpassfilter =
	__ATTR(lowpassfilter, S_IWUSR | S_IWGRP,
		NULL, inv_lowpassfilter_store);


static struct device_attribute *accel_sensor_attrs[] = {
	&dev_attr_acc_vendor,
	&dev_attr_acc_name,
	&dev_attr_acc_raw_data,
	&dev_attr_acc_calibration,
	&dev_attr_acc_reactive_alert,
	&dev_attr_acc_selftest,
	&dev_attr_acc_lowpassfilter,
	NULL,
};
#endif


static int inv_mpu_parse_dt(struct mpu_platform_data *data, struct device *dev)
{
	struct device_node *this_node= dev->of_node;
	enum of_gpio_flags flags;
	u32 temp;
	u32 orientation[9], i = 0;

	if (this_node == NULL)
		return -ENODEV;

	data->irq = of_get_named_gpio_flags(this_node,
						"inv,irq_gpio", 0, &flags);
	if (data->irq < 0) {
		pr_err("%s : get irq_gpio(%d) error\n", __func__, data->irq);
		return -ENODEV;
	}

	if (of_property_read_u32(this_node,
			"inv,int_config", &temp) < 0) {
		pr_err("%s : get int_config(%d) error\n", __func__, temp);
		return -ENODEV;
	} else {
		data->int_config = (u8)temp;
	}

	if (of_property_read_u32(this_node,
			"inv,level_shifter", &temp) < 0) {
		pr_err("%s : get level_shifter(%d) error\n", __func__, temp);
		return -ENODEV;
	} else {
		data->level_shifter = (u8)temp;
	}

	if (of_property_read_u32_array(this_node,
		"inv,orientation", orientation, 9) < 0) {
		pr_err("%s : get orientation(%d) error\n", __func__, orientation[0]);
		return -ENODEV;
	}

	for (i = 0 ; i < 9 ; i++)
		data->orientation[i] = ((s8)orientation[i]) - 1;

	return 0;
}

static int inv_mpu_pin(struct i2c_client *this, unsigned irq)
{
	int ret;

	ret = gpio_request(irq, "mpu_irq");
	if (ret < 0) {
		pr_err("%s - gpio %d request failed (%d)\n",
			__func__, irq, ret);
		return ret;
	}

	ret = gpio_direction_input(irq);
	if (ret < 0) {
		pr_err("%s - failed to set gpio %d as input (%d)\n",
			__func__, irq, ret);
		gpio_free(irq);
		return ret;
	}

	this->irq = gpio_to_irq(irq);
	pr_info("%s: %d, %d\n", __func__, this->irq, irq);
	return 0;
}

#if 1
static int inv_regulator_onoff(struct inv_mpu_state *st, bool onoff)
{
	int ret = 0;

	pr_info("%s %s\n", __func__, (onoff) ? "on" : "off");

	if (!st->reg_vdd) {
		pr_info("%s VDD get regulator\n", __func__);
		st->reg_vdd = devm_regulator_get(&st->client->dev,
			"inv,vdd");
		if (IS_ERR(st->reg_vdd)) {
			pr_err("could not get vdd, %ld\n",
				PTR_ERR(st->reg_vdd));
			ret = -ENODEV;
			goto err_vdd;
		}
		else if(!regulator_get_voltage(st->reg_vdd))
			regulator_set_voltage(st->reg_vdd, 2850000, 2850000);
	}

	if (onoff) {
		ret = regulator_enable(st->reg_vdd);
		if (ret)
			pr_err("%s: Failed to enable vdd.\n", __func__);
		msleep(10);
	} else {
		ret = regulator_disable(st->reg_vdd);
		if (ret)
			pr_err("%s: Failed to disable vdd.\n", __func__);
		msleep(10);
	}

	return 0;

err_vdd:

	return ret;
}
#endif
/*
 *  inv_mpu_probe() - probe function.
 */
static int inv_mpu_probe(struct i2c_client *client,
						const struct i2c_device_id *id)
{
	struct inv_mpu_state *st;
	struct iio_dev *indio_dev;
	int result;

	pr_info("%s: start....\n", __func__);

#ifdef CONFIG_DTS_INV_MPU_IIO
	enable_irq_wake(client->irq);
#endif

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		result = -ENOSYS;
		pr_err("%s: I2c function error\n", __func__);
		goto out_no_free;
	}
#ifdef LINUX_KERNEL_3_10
	indio_dev = iio_device_alloc(sizeof(*st));
#else
	indio_dev = iio_allocate_device(sizeof(*st));
#endif
	if (indio_dev == NULL) {
		pr_err("%s: memory allocation failed\n", __func__);
		result =  -ENOMEM;
		goto out_no_free;
	}
	st = iio_priv(indio_dev);
	st->client = client;

	pr_info("[INVN:%s] client->irq = %d\n", __func__, client->irq);
	st->sl_handle = client->adapter;
	st->i2c_addr = client->addr;
	st->i2c_dis = 0;
/*****************************************************************************/
/*      SysFS for Pedo logging mode features                                 */
	init_completion(&st->pedlog.wait);
	INIT_WORK(&st->pedlog.work, inv_pedlog_sched_work);
	init_timer(&st->pedlog.timer);
	st->pedlog.timer.data = (unsigned long)st;
	st->pedlog.timer.function = inv_pedlog_timer_func;
	st->pedlog.step_count = 0;
/*^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^*/

	inv_regulator_onoff(st, true);

#ifdef CONFIG_DTS_INV_MPU_IIO
	result = invensense_mpu_parse_dt(&client->dev, &st->plat_data);
	if (result)
		goto out_free;

	/*Power on device.*/
	if (st->plat_data.power_on) {
		result = st->plat_data.power_on(&st->plat_data);
		if (result < 0) {
			dev_err(&client->dev,
					"power_on failed: %d\n", result);
			return result;
		}
		pr_info("%s: power on here.\n", __func__);
	}
	pr_info("%s: power on.\n", __func__);

msleep(100);
#else
#if defined(CONFIG_SENSORS)
	result = inv_mpu_parse_dt(&st->plat_data, &client->dev);
	if (result) {
		dev_err(&client->adapter->dev,
			"Could not initialize device.\n");
		goto out_free;
	}

	result = inv_mpu_pin(client, (unsigned)st->plat_data.irq);
	if (result)
		goto out_free;
#else //CONFIG_SENSORS
	st->plat_data =
		*(struct mpu_platform_data *)dev_get_platdata(&client->dev);
#endif //CONFIG_SENSORS
#endif
	/* power is turned on inside check chip type*/
	result = inv_check_chip_type(indio_dev, id->name);
	if (result)
		goto out_free;

	/* Make state variables available to all _show and _store functions. */
	i2c_set_clientdata(client, indio_dev);
	indio_dev->dev.parent = &client->dev;
	indio_dev->name = id->name;
	st->irq = client->irq;

	result = inv_mpu_configure_ring(indio_dev);
	if (result) {
		INVLOG(ERR, "configure ring buffer fail\n");
		goto out_free;
	}
	enable_irq_wake(st->irq);
	result = iio_buffer_register(indio_dev, indio_dev->channels,
					indio_dev->num_channels);
	if (result) {
		INVLOG(ERR, "ring buffer register fail\n");
		goto out_unreg_ring;
	}
//	INV_I2C_SETIRQ(IRQ_MPU, client->irq);

	result = iio_device_register(indio_dev);
	if (result) {
		INVLOG(ERR, "IIO device register fail\n");
		goto out_remove_ring;
	}

#ifdef CONFIG_SENSORS
	wake_lock_init(&st->reactive_wake_lock, WAKE_LOCK_SUSPEND,"reactive_wake_lock");

	result = sensors_register(st->accel_sensor_device,
		st, accel_sensor_attrs, "accelerometer_sensor");
	if (result) {
		pr_err("%s: cound not register accel sensor device(%d).\n",
		__func__, result);
		goto err_accel_sensor_register_failed;
	}

	accel_open_calibration(st);
#endif

	result = inv_create_dmp_sysfs(indio_dev);
	if (result) {
		INVLOG(ERR, "create dmp sysfs failed\n");
		goto out_unreg_iio;
	}
	sema_init(&st->suspend_resume_sema, 1);
	dev_info(&client->dev, "%s is ready to go!\n", indio_dev->name);
	wake_lock_init(&st->pedlog.wake_lock, WAKE_LOCK_SUSPEND, "inv_iio");

	return 0;
out_unreg_iio:
	sensors_unregister(st->accel_sensor_device, accel_sensor_attrs);
err_accel_sensor_register_failed:
	iio_device_unregister(indio_dev);
out_remove_ring:
	iio_buffer_unregister(indio_dev);
out_unreg_ring:
	inv_mpu_unconfigure_ring(indio_dev);
out_free:
	inv_regulator_onoff(st, false);
#ifdef LINUX_KERNEL_3_10
	iio_device_free(indio_dev);
#else
	iio_free_device(indio_dev);
#endif
out_no_free:
	dev_err(&client->adapter->dev, "%s failed %d\n", __func__, result);

	return -EIO;
}

static void inv_mpu_shutdown(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct inv_mpu_state *st = iio_priv(indio_dev);
	int result;

	INVLOG(IL2, "Enter\n");
	mutex_lock(&indio_dev->mlock);
	inv_switch_power_in_lp(st, true);
	dev_dbg(&client->adapter->dev, "Shutting down %s...\n", st->hw->name);

	/* reset to make sure previous state are not there */
	result = inv_plat_single_write(st, REG_PWR_MGMT_1, BIT_H_RESET);
	if (result)
		dev_err(&client->adapter->dev, "Failed to reset %s\n",
			st->hw->name);
	msleep(POWER_UP_TIME);
	/* turn off power to ensure gyro engine is off */
	result = inv_set_power(st, false);
	if (result)
		dev_err(&client->adapter->dev, "Failed to turn off %s\n",
			st->hw->name);
	inv_switch_power_in_lp(st, false);
	mutex_unlock(&indio_dev->mlock);
	inv_regulator_onoff(st, false);
	INVLOG(FUNC_ENTRY, "Exit\n");
}

/*
 *  inv_mpu_remove() - remove function.
 */
static int inv_mpu_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	INVLOG(IL2, "Enter\n");
	iio_device_unregister(indio_dev);
	iio_buffer_unregister(indio_dev);
	inv_mpu_unconfigure_ring(indio_dev);
#ifdef LINUX_KERNEL_3_10
	iio_device_free(indio_dev);
#else
	iio_free_device(indio_dev);
#endif

	dev_info(&client->adapter->dev, "inv-mpu-iio module removed.\n");
	INVLOG(FUNC_ENTRY, "Exit\n");

	return 0;
}

#ifdef CONFIG_PM_SLEEP
/*
 * inv_mpu_resume(): resume method for this driver.
 *    This method can be modified according to the request of different
 *    customers. It basically undo everything suspend_noirq is doing
 *    and recover the chip to what it was before suspend.
 */
static int inv_mpu_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct inv_mpu_state *st = iio_priv(indio_dev);

	/* add code according to different request Start */
	INVLOG(IL2, "%s inv_mpu_resume\n", st->hw->name);

	if (st->chip_config.dmp_on) {
		mutex_lock(&indio_dev->mlock);
		inv_switch_power_in_lp(st, true);
		if (st->batch.on) {
			INVLOG(IL4, "DMP is on.\n");
			write_be32_to_mem(st, st->batch.counter, BM_BATCH_THLD);
			inv_plat_single_write(st, REG_INT_ENABLE_2,
							BIT_FIFO_OVERFLOW_EN_0);
		}
		inv_restore_state(st);
		inv_check_sensor_on(st);
		inv_check_sensor_rate(st);
		set_inv_enable(indio_dev);
		inv_enable_pedlog_interrupt(st, false);
		inv_enable_pedometer_interrupt(st, true);
		inv_switch_power_in_lp(st, false);
		mutex_unlock(&indio_dev->mlock);
	} else {
		INVLOG(IL4, "DMP is off.\n");
	}
	/* add code according to different request End */
	INVLOG(FUNC_ENTRY, "Exit\n");

	return 0;
}

/*
 * inv_mpu_suspend(): suspend method for this driver.
 *    This method can be modified according to the request of different
 *    customers. If customer want some events, such as SMD to wake up the CPU,
 *    then data interrupt should be disabled in this interrupt to avoid
 *    unnecessary interrupts. If customer want pedometer running while CPU is
 *    asleep, then pedometer should be turned on while pedometer interrupt
 *    should be turned off.
 */
static int inv_mpu_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct inv_mpu_state *st = iio_priv(indio_dev);
	int i;

	/* add code according to different request Start */
	INVLOG(IL2, "%s inv_mpu_suspend\n", st->hw->name);
	mutex_lock(&indio_dev->mlock);

	inv_switch_power_in_lp(st, true);
	if (st->chip_config.dmp_on) {
		INVLOG(IL4, "DMP is on.\n");
		if (!st->chip_config.wake_on) {
			INVLOG(IL4, "Wake is off.\n");
			if (st->batch.on) {
				INVLOG(IL4, "Batch is on.\n");
				write_be32_to_mem(st, INT_MAX, BM_BATCH_THLD);
				inv_plat_single_write(st, REG_INT_ENABLE_2, 0);
			}
		}
		inv_store_state(st);
		for(i = 0 ; i < SENSOR_L_NUM_MAX ; i++)
			st->sensor_l[i].on = false;
		st->trigger_state = RATE_TRIGGER;
		inv_check_sensor_on(st);
		inv_check_sensor_rate(st);
		set_inv_enable(indio_dev);
		if(st->wom_enable)
		{
			INVLOG(IL2, "data %d\n", true);
			inv_wom_enable(st, true);
		}
		inv_enable_pedometer_interrupt(st, false);
		if(st->pedlog.enabled)
		{
			inv_enable_pedlog_interrupt(st, true);
		}
	} else {
		/* in non DMP case, just turn off the power */
		INVLOG(IL4, "DMP is off.\n");
		inv_set_power(st, false);
	}
	inv_switch_power_in_lp(st, false);
	mutex_unlock(&indio_dev->mlock);

	/* add code according to different request End */

	INVLOG(FUNC_ENTRY, "Exit\n");
	return 0;
}

inv_error_t inv_suspend_enable(struct device *dev, unsigned char enable)
{
	INVLOG(FUNC_ENTRY, "Enter\n");

	if(!!enable)
	{
		INVLOG(IL4, "Go to suspend\n");
		inv_mpu_suspend(dev);
	}
	else
	{
		INVLOG(IL4, "Go to resume\n");
		inv_mpu_resume(dev);
	}

	INVLOG(FUNC_ENTRY, "Exit\n");
	return INV_SUCCESS;
}

static const struct dev_pm_ops inv_mpu_pmops = {
	.suspend       = inv_mpu_suspend,
	.resume        = inv_mpu_resume,
};
#define INV_MPU_PMOPS (&inv_mpu_pmops)
#else
#define INV_MPU_PMOPS NULL
#endif /* CONFIG_PM */

static const u16 normal_i2c[] = { I2C_CLIENT_END };
/* device id table is used to identify what device can be
 * supported by this driver
 */
static const struct i2c_device_id inv_mpu_id[] = {
#ifdef CONFIG_DTS_INV_MPU_IIO
	{"mpu6515", ICM20645},
#else
	{"mpu7400", ICM20645},
#endif
	{"icm20645", ICM20645},
	{"icm10320", ICM10320},
	{}
};

MODULE_DEVICE_TABLE(i2c, inv_mpu_id);

static struct i2c_driver inv_mpu_driver = {
	.class = I2C_CLASS_HWMON,
	.probe		=	inv_mpu_probe,
	.remove		=	inv_mpu_remove,
	.shutdown	=	inv_mpu_shutdown,
	.id_table	=	inv_mpu_id,
	.driver = {
		.owner	=	THIS_MODULE,
		.name	=	"inv-mpu-iio",
		.pm     =       INV_MPU_PMOPS,
	},
	.address_list = normal_i2c,
};
#ifdef LINUX_KERNEL_3_10
module_i2c_driver(inv_mpu_driver);
#else
static int __init inv_mpu_init(void)
{
	int result = i2c_add_driver(&inv_mpu_driver);
	if (result) {
		pr_err("failed\n");
		return result;
	}
	return 0;
}

static void __exit inv_mpu_exit(void)
{
	i2c_del_driver(&inv_mpu_driver);
}

module_init(inv_mpu_init);
module_exit(inv_mpu_exit);
#endif

MODULE_AUTHOR("Invensense Corporation");
MODULE_DESCRIPTION("Invensense device driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("inv-mpu-iio");
