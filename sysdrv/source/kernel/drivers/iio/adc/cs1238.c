// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cs1238: analog to digital converter for weight sensor module
 *
 * Copyright (c) 2016 Andreas Klinger <ak@it-klinger.de>
 */
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

/* gain to pulse and scale conversion */
#define cs1238_GAIN_MAX		3
#define cs1238_RESET_GAIN	128


#define CS1238_CHANNEL_A	0
#define CS1238_CHANNEL_B	1

#define CS1238_PGA_1		0 << 2
#define CS1238_PGA_2		1 << 2
#define CS1238_PGA_64		2 << 2
#define CS1238_PGA_128		3 << 2
#define CS1238_PGA_NUM		4

#define CS1238_SPEED_10HZ	 	0 << 4
#define CS1238_SPEED_40HZ		1 << 4
#define CS1238_SPEED_640HZ		2 << 4
#define CS1238_SPEED_1280HZ		3 << 4
#define CS1238_SPEED_NUM		4

#define CS1238_REF_ON		0 << 6
#define CS1238_REF_OFF		1 << 6

#define CS1238_RESERVE		0 << 7

#define CS1238_REG_WRITE    0x65
#define CS1238_REG_READ     0x56 

u8 cs1238_state = 0;
u8 cs1238_state_ch0 = 0;
u8 cs1238_state_ch1 = 0;

/*
 * .scale depends on AVDD which in turn is known as soon as the regulator
 * is available
 * therefore we set .scale in cs1238_probe()
 *
 * channel A in documentation is channel 0 in source code
 * channel B in documentation is channel 1 in source code
 */

//  struct cs1238_gain_to_scale {
// 	int			gain;
// 	int			gain_pulse;
// 	int			scale;
// 	int			channel;
// };


// static struct cs1238_gain_to_scale cs1238_gain_to_scale[cs1238_GAIN_MAX] = {
// 	{ 128, 1, 0, 0 },
// 	{  32, 2, 0, 1 },
// 	{  64, 3, 0, 0 }
// };


int freq[4]={10,40,640,1280};
int pga[4]={1,2,64,128};
int  scale[4];
// static int cs1238_get_gain_to_pulse(int gain)
// {
// 	int i;

// 	for (i = 0; i < cs1238_GAIN_MAX; i++)
// 		if (cs1238_gain_to_scale[i].gain == gain)
// 			return cs1238_gain_to_scale[i].gain_pulse;
// 	return 1;
// }

// static int cs1238_get_gain_to_scale(int gain)
// {
// 	int i;

// 	for (i = 0; i < cs1238_GAIN_MAX; i++)
// 		if (cs1238_gain_to_scale[i].gain == gain)
// 			return cs1238_gain_to_scale[i].scale;
// 	return 0;
// }

// static int cs1238_get_scale_to_gain(int scale)
// {
// 	int i;

// 	for (i = 0; i < cs1238_GAIN_MAX; i++)
// 		if (cs1238_gain_to_scale[i].scale == scale)
// 			return cs1238_gain_to_scale[i].gain;
// 	return -EINVAL;
// }

struct cs1238_data {
	struct device		*dev;
	struct gpio_desc	*gpiod_pd_sck;
	struct gpio_desc	*gpiod_dout;
	struct regulator	*reg_avdd;
	int			gain_set;	/* gain set on device */
	int			gain_chan_a;	/* gain for channel A */
	int			gain_chan_b;	/* gain for channel A */
	struct mutex		lock;
	/*
	 * triggered buffer
	 * 2x32-bit channel + 64-bit naturally aligned timestamp
	 */
	u32			buffer[4] __aligned(8);
	/*
	 * delay after a rising edge on SCK until the data is ready DOUT
	 * this is dependent on the cs1238 where the datasheet tells a
	 * maximum value of 100 ns
	 * but also on potential parasitic capacities on the wiring
	 */
	u32			data_ready_delay_ns;
	u32			clock_frequency;
	
	int  		avdd_uv;
};

static int cs1238_cycle(struct cs1238_data *cs1238_data)
{
	// unsigned long flags;
	/*
	 * if preempted for more then 60us while PD_SCK is high:
	 * cs1238 is going in reset
	 * ==> measuring is false
	 */
	gpiod_set_value(cs1238_data->gpiod_pd_sck, 1);
	/*
	 * wait until DOUT is ready
	 * it turned out that parasitic capacities are extending the time
	 * until DOUT has reached it's value
	 */
	ndelay(cs1238_data->data_ready_delay_ns);
	/*
	 * here we are not waiting for 0.2 us as suggested by the datasheet,
	 * because the oscilloscope showed in a test scenario
	 * at least 1.15 us for PD_SCK high (T3 in datasheet)
	 * and 0.56 us for PD_SCK low on TI Sitara with 800 MHz
	 */
	gpiod_set_value(cs1238_data->gpiod_pd_sck, 0);

	/*
	 * make it a square wave for addressing cases with capacitance on
	 * PC_SCK
	 */
	ndelay(cs1238_data->data_ready_delay_ns);

	/* sample as late as possible */
	return 0;
}


static int cs1238_cycle_read(struct cs1238_data *cs1238_data)
{	
	cs1238_cycle(cs1238_data);
	/* sample as late as possible */
	return gpiod_get_value(cs1238_data->gpiod_dout);
}


static int cs1238_cycle_write(struct cs1238_data *cs1238_data,int value)
{
	gpiod_set_value(cs1238_data->gpiod_dout, value);
	/* sample as late as possible */
	return cs1238_cycle(cs1238_data);
}

static int cs1238_wait_for_ready(struct cs1238_data *cs1238_data)
{
	int i, val;

	for (i = 0; i < 100; i++) {
		val = gpiod_get_value(cs1238_data->gpiod_dout);
		if (!val)
			break;
		/* sleep at least 10 ms */
		msleep(10);
	}
	if (val)
		return -EIO;

	return 0;
}

static int cs1238_read(struct cs1238_data *cs1238_data)
{
	int i, ret;
	int value = 0;
	int val = gpiod_get_value(cs1238_data->gpiod_dout);
	u8 cfg=0;
	u8 update=0;
	u8 reg = CS1238_REG_READ;
	
	// gpiod_direction_output(cs1238_data->gpiod_dout, 1);
	gpiod_direction_input(cs1238_data->gpiod_dout);

	val = cs1238_wait_for_ready(cs1238_data);
	/* we double check if it's really down */
	if (val)
		return -EIO;

	/* read 24 bits  0-23*/
	for (i = 0; i < 24; i++) {
		value <<= 1;
		ret = cs1238_cycle_read(cs1238_data);
		if (ret)
			value++;
	}

	// update config 24-25
	if(cs1238_cycle_read(cs1238_data))
		update++;
	update <<= 1;
	if(cs1238_cycle_read(cs1238_data))
		update++;

	// set dout input 26
	cs1238_cycle_read(cs1238_data);

	// set dout to high 27-28
	cs1238_cycle_read(cs1238_data);
	cs1238_cycle_read(cs1238_data);

	gpiod_direction_output(cs1238_data->gpiod_dout, 0);
	
	// write register 29-35
	for (i = 1; i < 8; i++){
		reg <<= 1;
		if(reg & 0x80)
			val=1;
		else
			val=0;
		cs1238_cycle_write(cs1238_data,val);
	}
	
	gpiod_direction_output(cs1238_data->gpiod_dout, 0);

	ndelay(cs1238_data->data_ready_delay_ns*2);
	ndelay(cs1238_data->data_ready_delay_ns*2);

	// set direction to input 36
	gpiod_direction_input(cs1238_data->gpiod_dout);
	cs1238_cycle_read(cs1238_data);

	//read config 37-44
	for(i=0;i<8;i++){
		cfg <<= 1;
		ret = cs1238_cycle_read(cs1238_data);
		if (ret)
			cfg++;
	}

	cs1238_cycle_read(cs1238_data);

	value ^= 0x800000;
	cs1238_state = cfg;

	// printk("cs1238: value: %d %x cfg=%x\n", value,value ,cfg);

	return value;
}


static int cs1238_write_cfg(struct cs1238_data *cs1238_data,u8 cs1238_cfg)
{
	int i;
	int val = gpiod_get_value(cs1238_data->gpiod_dout);
	u8 cfg=cs1238_cfg;
	u8 update=0;
	u8 reg = CS1238_REG_WRITE;
	// printk("cs1238: write cfg %x\n",cs1238_cfg);

	gpiod_direction_input(cs1238_data->gpiod_dout);

	/* we double check if it's really down */
	val = cs1238_wait_for_ready(cs1238_data);
	if (val)
		return -EIO;

	/* read 24 bits  0-23*/
	for (i = 0; i < 24; i++) {
		cs1238_cycle_read(cs1238_data);
	}

	// update config 24-25
	if(cs1238_cycle_read(cs1238_data))
		update++;
	update <<= 1;
	if(cs1238_cycle_read(cs1238_data))
		update++;

	// set dout input 26
	cs1238_cycle_read(cs1238_data);

	// set dout to high 27-28
	cs1238_cycle_read(cs1238_data);
	cs1238_cycle_read(cs1238_data);

	gpiod_direction_output(cs1238_data->gpiod_dout, 0);
	// write register 29-35
	for (i = 1; i < 8; i++){
		reg <<= 1;
		if(reg & 0x80)
			val=1;
		else
			val=0;
		cs1238_cycle_write(cs1238_data,val);
	}
	
	gpiod_direction_output(cs1238_data->gpiod_dout, 0);

	ndelay(cs1238_data->data_ready_delay_ns*2);
	ndelay(cs1238_data->data_ready_delay_ns*2);

	// set direction to input 36
	gpiod_direction_output(cs1238_data->gpiod_dout, 0);
	cs1238_cycle_write(cs1238_data,0);

	//read config 37-44
	for(i=0;i<8;i++){
		if(cfg & 0x80)
			val=1;
		else
			val=0;
		cs1238_cycle_write(cs1238_data,val);
		cfg <<= 1;

	}
	gpiod_direction_input(cs1238_data->gpiod_dout);
	cs1238_cycle_read(cs1238_data);

	return 0;
}

static int cs1238_set_channel(struct cs1238_data *cs1238_data, int chan)
{
	int ret;

	// right chanel
	if((cs1238_state & CS1238_CHANNEL_B)  != chan){
		ret=0;
		// set chanel
		// printk("cs1238: set channel %d\n",chan);
		if(chan){
			ret = cs1238_write_cfg(cs1238_data,cs1238_state_ch1);
		}else{
			ret = cs1238_write_cfg(cs1238_data,cs1238_state_ch0);
		}
	}

	return 0;
}

static int cs1238_read_ch(struct cs1238_data *cs1238_data, int chan)
{
	int ret;
	int val;

	ret = cs1238_set_channel(cs1238_data, chan);
	if (ret < 0)
		return ret;
	val = cs1238_read(cs1238_data);

	return val;
}

static int cs1238_read_raw(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				int *val, int *val2, long mask)
{
	struct cs1238_data *cs1238_data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&cs1238_data->lock);

		*val = cs1238_read_ch(cs1238_data, chan->channel);

		mutex_unlock(&cs1238_data->lock);

		if (*val < 0)
			return *val;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		mutex_lock(&cs1238_data->lock);
		if (chan->channel){
			*val2 = scale[(cs1238_state_ch1 & CS1238_PGA_128)>>2];
		}
		else{
			*val2 = scale[(cs1238_state_ch0 & CS1238_PGA_128)>>2];
		}

		mutex_unlock(&cs1238_data->lock);

		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&cs1238_data->lock);
		if (chan->channel){
			*val = freq[(cs1238_state_ch1 & CS1238_SPEED_1280HZ)>>4];
			printk("cs1238: %x,%x\n",cs1238_state_ch1,(cs1238_state_ch1 & CS1238_SPEED_1280HZ)>>4);
		}
		else{
			*val = freq[(cs1238_state_ch0 & CS1238_SPEED_1280HZ)>>4];
			printk("cs1238: %x,%x\n",cs1238_state_ch0,(cs1238_state_ch0 & CS1238_SPEED_1280HZ)>>4);
		}

		mutex_unlock(&cs1238_data->lock);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int cs1238_write_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int val,
				int val2,
				long mask)
{
	struct cs1238_data *cs1238_data = iio_priv(indio_dev);
	// int ret;
	u8 gain;
	u8 i;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		if (val != 0)
			return -EINVAL;

		mutex_lock(&cs1238_data->lock);

		for(i=0;i<CS1238_PGA_NUM;i++){
			if(val2 == scale[i]){
				gain=i;
				break;
			}
		}
		if(i==CS1238_PGA_NUM)
			return -EINVAL;

		printk("cs1238: set gain %d\n",gain);

		if (chan->channel){
		    cs1238_state_ch1 = (cs1238_state_ch1 & ~(CS1238_PGA_128)  )| (gain<<2);
			cs1238_write_cfg(cs1238_data,cs1238_state_ch1);
		}else{
		    cs1238_state_ch0 = (cs1238_state_ch0 & ~(CS1238_PGA_128) )| (gain<<2);
			cs1238_write_cfg(cs1238_data,cs1238_state_ch0);
		}

		mutex_unlock(&cs1238_data->lock);
		return 0;
	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&cs1238_data->lock);

		for(i=0;i<CS1238_SPEED_NUM;i++){
			if(val == freq[i]){
				gain=i;
				break;
			}
		}
		if(i==CS1238_SPEED_NUM)
			return -EINVAL;

		printk("cs1238: set freq %d\n",gain);

		if (chan->channel){
			cs1238_state_ch1=(~(CS1238_SPEED_1280HZ) & cs1238_state_ch1) | (gain<<4);
			printk("cs1238: %x\n",cs1238_state_ch1);
			cs1238_write_cfg(cs1238_data,cs1238_state_ch1);
		}else{
		    cs1238_state_ch0=(~(CS1238_SPEED_1280HZ) & cs1238_state_ch0) | (gain<<4);
			printk("cs1238: %x\n",cs1238_state_ch0);
			cs1238_write_cfg(cs1238_data,cs1238_state_ch0);
		}

		mutex_unlock(&cs1238_data->lock);
		return 0;
	default:
		return -EINVAL;
	}

	return 0;
}

static int cs1238_write_raw_get_fmt(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		long mask)
{
	return IIO_VAL_INT_PLUS_NANO;
}

static ssize_t cs1238_scale_available_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	// struct iio_dev_attr *iio_attr = to_iio_dev_attr(attr);
	// int channel = iio_attr->address;
	int i, len = 0;

	for (i = 0; i < CS1238_PGA_NUM; i++)
		len += sprintf(buf + len, "0.%09d ",scale[i]);

	len += sprintf(buf + len, "\n");

	return len;
}

static ssize_t cs1238_sampling_frequency_available_show(struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	// struct iio_dev_attr *iio_attr = to_iio_dev_attr(attr);
	// int channel = iio_attr->address;
	int i, len = 0;

	for (i = 0; i < CS1238_SPEED_NUM; i++)
	len += sprintf(buf + len, "%d ",freq[i]);

	len += sprintf(buf + len, "\n");

	return len;
}

static IIO_DEVICE_ATTR(in_voltage0_scale_available, S_IRUGO,
	cs1238_scale_available_show, NULL, 0);

static IIO_DEVICE_ATTR(in_voltage1_scale_available, S_IRUGO,
	cs1238_scale_available_show, NULL, 1);

static IIO_DEVICE_ATTR(in_voltage0_sampling_frequency_available, S_IRUGO,
	cs1238_sampling_frequency_available_show, NULL, 1);

static IIO_DEVICE_ATTR(in_voltage1_sampling_frequency_available, S_IRUGO,
	cs1238_sampling_frequency_available_show, NULL, 1);

static struct attribute *cs1238_attributes[] = {
	&iio_dev_attr_in_voltage0_scale_available.dev_attr.attr,
	&iio_dev_attr_in_voltage1_scale_available.dev_attr.attr,
	&iio_dev_attr_in_voltage0_sampling_frequency_available.dev_attr.attr,
	&iio_dev_attr_in_voltage1_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group cs1238_attribute_group = {
	.attrs = cs1238_attributes,
};

static const struct iio_info cs1238_iio_info = {
	.read_raw		= cs1238_read_raw,
	.write_raw		= cs1238_write_raw,
	.write_raw_get_fmt	= cs1238_write_raw_get_fmt,
	.attrs			= &cs1238_attribute_group,
};

static const struct iio_chan_spec cs1238_chan_spec[] = {
	{
		.type = IIO_VOLTAGE,
		.channel = 0,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE)|BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_index = 0,
		.scan_type = {
			.sign = 'u',
			.realbits = 24,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	{
		.type = IIO_VOLTAGE,
		.channel = 1,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE)|BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_index = 1,
		.scan_type = {
			.sign = 'u',
			.realbits = 24,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(2),
};

static int cs1238_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct cs1238_data *cs1238_data;
	struct iio_dev *indio_dev;
	int ret;
	int i;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct cs1238_data));
	if (!indio_dev) {
		dev_err(dev, "failed to allocate IIO device\n");
		return -ENOMEM;
	}

	cs1238_data = iio_priv(indio_dev);
	cs1238_data->dev = dev;

	mutex_init(&cs1238_data->lock);

	/*
	 * PD_SCK stands for power down and serial clock input of cs1238
	 * in the driver it is an output
	 */
	cs1238_data->gpiod_pd_sck = devm_gpiod_get(dev, "sck", GPIOD_OUT_LOW);
	if (IS_ERR(cs1238_data->gpiod_pd_sck)) {
		dev_err(dev, "failed to get sck-gpiod: err=%ld\n",
					PTR_ERR(cs1238_data->gpiod_pd_sck));
		return PTR_ERR(cs1238_data->gpiod_pd_sck);
	}

	/*
	 * DOUT stands for serial data output of cs1238
	 * for the driver it is an input
	 */
	cs1238_data->gpiod_dout = devm_gpiod_get(dev, "dout", GPIOD_IN);
	if (IS_ERR(cs1238_data->gpiod_dout)) {
		dev_err(dev, "failed to get dout-gpiod: err=%ld\n",
					PTR_ERR(cs1238_data->gpiod_dout));
		return PTR_ERR(cs1238_data->gpiod_dout);

	}

	cs1238_data->reg_avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(cs1238_data->reg_avdd))
		return PTR_ERR(cs1238_data->reg_avdd);

	ret = regulator_enable(cs1238_data->reg_avdd);
	if (ret < 0)
		return ret;

	cs1238_data->avdd_uv = regulator_get_voltage(cs1238_data->reg_avdd);
	if (ret < 0)
		goto error_regulator;


	// PGA 1
	for(i=0;i<4;i++){
		scale[i] = cs1238_data->avdd_uv/10000*596/pga[i];
		printk("scale[%d] = %d\n",i,scale[i]);
	}

	cs1238_data->clock_frequency = 400000;
	ret = of_property_read_u32(np, "clock-frequency",
					&cs1238_data->clock_frequency);

	/*
	 * datasheet says the high level of PD_SCK has a maximum duration
	 * of 50 microseconds
	 */
	if (cs1238_data->clock_frequency < 10000) {
		dev_warn(dev, "clock-frequency too low - assuming 400 kHz\n");
		cs1238_data->clock_frequency = 400000;
	}

	
	cs1238_state_ch0 = CS1238_CHANNEL_A | CS1238_PGA_128 | CS1238_SPEED_10HZ | CS1238_REF_ON | CS1238_RESERVE;
	cs1238_state_ch1 = CS1238_CHANNEL_B | CS1238_PGA_128 | CS1238_SPEED_10HZ | CS1238_REF_ON | CS1238_RESERVE;
	cs1238_state = cs1238_state_ch0; 

	cs1238_data->data_ready_delay_ns =
				1000000000 / cs1238_data->clock_frequency;

	platform_set_drvdata(pdev, indio_dev);

	indio_dev->name = "cs1238";
	indio_dev->info = &cs1238_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = cs1238_chan_spec;
	indio_dev->num_channels = ARRAY_SIZE(cs1238_chan_spec);

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(dev, "Couldn't register the device\n");
		goto error_buffer;
	}

	return 0;

error_buffer:
	iio_triggered_buffer_cleanup(indio_dev);

error_regulator:
	regulator_disable(cs1238_data->reg_avdd);

	return ret;
}

static int cs1238_remove(struct platform_device *pdev)
{
	struct cs1238_data *cs1238_data;
	struct iio_dev *indio_dev;

	indio_dev = platform_get_drvdata(pdev);
	cs1238_data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	iio_triggered_buffer_cleanup(indio_dev);

	regulator_disable(cs1238_data->reg_avdd);

	return 0;
}

static const struct of_device_id of_cs1238_match[] = {
	{ .compatible = "chipsea,cs1238", },
	{},
};

MODULE_DEVICE_TABLE(of, of_cs1238_match);

static struct platform_driver cs1238_driver = {
	.probe		= cs1238_probe,
	.remove		= cs1238_remove,
	.driver		= {
		.name		= "cs1238-gpio",
		.of_match_table	= of_cs1238_match,
	},
};

module_platform_driver(cs1238_driver);

MODULE_AUTHOR("Andreas Klinger <ak@it-klinger.de>");
MODULE_DESCRIPTION("cs1238 bitbanging driver - ADC for weight cells");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:cs1238-gpio");

