/*
 * Copyright (c) 2017 Jean-Paul Etienne <fractalclone@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file GPIO driver for the SiFive Freedom Processor
 */

#include <errno.h>
#include <kernel.h>
#include <device.h>
#include <soc.h>
#include <drivers/gpio.h>
#include <sys/util.h>

#include "gpio_utils.h"

typedef void (*sifive_cfg_func_t)(void);

/* sifive GPIO register-set structure */
struct gpio_sifive_t {
	unsigned int in_val;
	unsigned int in_en;
	unsigned int out_en;
	unsigned int out_val;
	unsigned int pue;
	unsigned int ds;
	unsigned int rise_ie;
	unsigned int rise_ip;
	unsigned int fall_ie;
	unsigned int fall_ip;
	unsigned int high_ie;
	unsigned int high_ip;
	unsigned int low_ie;
	unsigned int low_ip;
	unsigned int iof_en;
	unsigned int iof_sel;
	unsigned int invert;
};

struct gpio_sifive_config {
	uintptr_t            gpio_base_addr;
	u32_t                plic_irq;
	u32_t                gpio_irq_base;
	sifive_cfg_func_t    gpio_cfg_func;
};

struct gpio_sifive_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	/* list of callbacks */
	sys_slist_t cb;
};

/* Helper Macros for GPIO */
#define DEV_GPIO_CFG(dev)						\
	((const struct gpio_sifive_config * const)(dev)->config->config_info)
#define DEV_GPIO(dev)							\
	((volatile struct gpio_sifive_t *)(DEV_GPIO_CFG(dev))->gpio_base_addr)
#define DEV_GPIO_DATA(dev)				\
	((struct gpio_sifive_data *)(dev)->driver_data)
#define DEV_GPIO_IRQ(irq, plic_irq)			\
	(((irq) << 8) | (plic_irq))

static void gpio_sifive_irq_handler(void *arg)
{
	struct device *dev = (struct device *)arg;
	struct gpio_sifive_data *data = DEV_GPIO_DATA(dev);
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);
	const struct gpio_sifive_config *cfg = DEV_GPIO_CFG(dev);
	int pin_mask;

	/* Get the pin number generating the interrupt */
	pin_mask = 1 << (riscv_plic_get_irq() - cfg->gpio_irq_base);
	printk("gpio_sifive_irq_handler: plic_irq %d\n", riscv_plic_get_irq());

	/*
	 * Write to either the rise_ip, fall_ip, high_ip or low_ip registers
	 * to indicate to GPIO controller that interrupt for the corresponding
	 * pin has been handled.
	 */
	if (gpio->rise_ip & pin_mask) {
		gpio->rise_ip = pin_mask;
	} else if (gpio->fall_ip & pin_mask) {
		gpio->fall_ip = pin_mask;
	} else if (gpio->high_ip & pin_mask) {
		gpio->high_ip = pin_mask;
	} else if (gpio->low_ip & pin_mask) {
		gpio->low_ip = pin_mask;
	}

	/* Call the corresponding callback registered for the pin */
	gpio_fire_callbacks(&data->cb, dev, pin_mask);
}

/**
 * @brief Configure pin
 *
 * @param dev Device structure
 * @param access_op Access operation
 * @param pin The pin number
 * @param flags Flags of pin or port
 *
 * @return 0 if successful, failed otherwise
 */
static int gpio_sifive_config(struct device *dev,
			     int access_op,
			     u32_t pin,
			     int flags)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	if (access_op != GPIO_ACCESS_BY_PIN) {
		return -ENOTSUP;
	}

	if (pin >= SIFIVE_PINMUX_PINS) {
		return -EINVAL;
	}

	/* We cannot support open-source open-drain configuration */
	if ((flags & GPIO_SINGLE_ENDED) != 0) {
		return -ENOTSUP;
	}

	/* We only support pull-ups, not pull-downs */
	if ((flags & GPIO_PULL_DOWN) != 0) {
		return -ENOTSUP;
	}

	/* Set pull-up if requested */
	WRITE_BIT(gpio->pue, pin, flags & GPIO_PULL_UP);

	/* Set the initial output value before enabling output to avoid
	 * glitches
	 */
	if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
		gpio->out_val |= BIT(pin);
	}
	if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
		gpio->out_val &= ~BIT(pin);
	}

	/* Enable input/output */
	WRITE_BIT(gpio->out_en, pin, flags & GPIO_OUTPUT);
	WRITE_BIT(gpio->in_en, pin, flags & GPIO_INPUT);

	return 0;
}

/**
 * @brief Set the pin
 *
 * @param dev Device struct
 * @param access_op Access operation
 * @param pin The pin number
 * @param value Value to set (0 or 1)
 *
 * @return 0 if successful, failed otherwise
 */
static int gpio_sifive_write(struct device *dev,
			    int access_op,
			    u32_t pin,
			    u32_t value)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	if (access_op != GPIO_ACCESS_BY_PIN) {
		return -ENOTSUP;
	}

	if (pin >= SIFIVE_PINMUX_PINS)
		return -EINVAL;

	/* If pin is configured as input return with error */
	if (gpio->in_en & BIT(pin)) {
		return -EINVAL;
	}

	if (value) {
		gpio->out_val |= BIT(pin);
	} else {
		gpio->out_val &= ~BIT(pin);
	}

	return 0;
}

/**
 * @brief Read the pin
 *
 * @param dev Device struct
 * @param access_op Access operation
 * @param pin The pin number
 * @param value Value of input pin(s)
 *
 * @return 0 if successful, failed otherwise
 */
static int gpio_sifive_read(struct device *dev,
			   int access_op,
			   u32_t pin,
			   u32_t *value)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	if (access_op != GPIO_ACCESS_BY_PIN) {
		return -ENOTSUP;
	}

	if (pin >= SIFIVE_PINMUX_PINS) {
		return -EINVAL;
	}

	/*
	 * If gpio is configured as output,
	 * read gpio value from out_val register,
	 * otherwise read gpio value from in_val register
	 */
	if (gpio->out_en & BIT(pin)) {
		*value = !!(gpio->out_val & BIT(pin));
	} else {
		*value = !!(gpio->in_val & BIT(pin));
	}

	return 0;
}

static int gpio_sifive_port_get_raw(struct device *dev,
				   gpio_port_value_t *value)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	*value = gpio->in_val;

	return 0;
}

static int gpio_sifive_port_set_masked_raw(struct device *dev,
					  gpio_port_pins_t mask,
					  gpio_port_value_t value)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	gpio->out_val = (gpio->out_val & ~mask) | (value & mask);

	return 0;
}

static int gpio_sifive_port_set_bits_raw(struct device *dev,
					gpio_port_pins_t mask)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	gpio->out_val |= mask;

	return 0;
}

static int gpio_sifive_port_clear_bits_raw(struct device *dev,
					  gpio_port_pins_t mask)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	gpio->out_val &= ~mask;

	return 0;
}

static int gpio_sifive_port_toggle_bits(struct device *dev,
				       gpio_port_pins_t mask)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);

	gpio->out_val ^= mask;

	return 0;
}

static int gpio_sifive_pin_interrupt_configure(struct device *dev,
					      unsigned int pin,
					      enum gpio_int_mode mode,
					      enum gpio_int_trig trig)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);
	const struct gpio_sifive_config *cfg = DEV_GPIO_CFG(dev);

	switch (mode) {
	case GPIO_INT_MODE_DISABLED:
		gpio->rise_ie &= ~BIT(pin);
		gpio->rise_ie &= ~BIT(pin);
		gpio->fall_ie &= ~BIT(pin);
		gpio->high_ie &= ~BIT(pin);
		gpio->low_ie &= ~BIT(pin);
		printk("Disabling irq 0x%x\n", DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
		irq_disable(DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
		break;
	case GPIO_INT_MODE_LEVEL:
		gpio->rise_ie &= ~BIT(pin);
		gpio->rise_ie &= ~BIT(pin);
		gpio->fall_ie &= ~BIT(pin);

		if (trig == GPIO_INT_TRIG_HIGH) {
			gpio->high_ie |= BIT(pin);
			gpio->low_ie &= ~BIT(pin);
		} else if (trig == GPIO_INT_TRIG_LOW) {
			gpio->high_ie &= ~BIT(pin);
			gpio->low_ie |= BIT(pin);
		}
		printk("Enabling irq 0x%x\n", DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
		irq_enable(DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
		break;
	case GPIO_INT_MODE_EDGE:
		gpio->high_ie &= ~BIT(pin);
		gpio->low_ie &= ~BIT(pin);

		/* Rising Edge, Falling Edge or Double Edge ? */
		if (trig == GPIO_INT_TRIG_HIGH) {
			gpio->rise_ie |= BIT(pin);
			gpio->fall_ie &= ~BIT(pin);
		} else if (trig == GPIO_INT_TRIG_LOW) {
			gpio->rise_ie &= ~BIT(pin);
			gpio->fall_ie |= BIT(pin);
		} else {
			gpio->rise_ie |= BIT(pin);
			gpio->fall_ie |= BIT(pin);
		}
		printk("Enabling irq 0x%x\n", DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
		irq_enable(DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
		break;
	}

	return 0;
}

static int gpio_sifive_manage_callback(struct device *dev,
				      struct gpio_callback *callback,
				      bool set)
{
	struct gpio_sifive_data *data = DEV_GPIO_DATA(dev);

	return gpio_manage_callback(&data->cb, callback, set);
}

static int gpio_sifive_enable_callback(struct device *dev,
				      int access_op,
				      u32_t pin)
{
	const struct gpio_sifive_config *cfg = DEV_GPIO_CFG(dev);

	if (access_op != GPIO_ACCESS_BY_PIN) {
		return -ENOTSUP;
	}

	if (pin >= SIFIVE_PINMUX_PINS) {
		return -EINVAL;
	}

	/* Enable interrupt for the pin at PLIC level */
	printk("Enabling irq 0x%x\n", DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
	irq_enable(DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));

	return 0;
}

static int gpio_sifive_disable_callback(struct device *dev,
				       int access_op,
				       u32_t pin)
{
	const struct gpio_sifive_config *cfg = DEV_GPIO_CFG(dev);

	if (access_op != GPIO_ACCESS_BY_PIN) {
		return -ENOTSUP;
	}

	if (pin >= SIFIVE_PINMUX_PINS) {
		return -EINVAL;
	}

	/* Disable interrupt for the pin at PLIC level */
	printk("Disabling irq 0x%x\n", DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));
	irq_disable(DEV_GPIO_IRQ(cfg->gpio_irq_base + pin, cfg->plic_irq));

	return 0;
}

static const struct gpio_driver_api gpio_sifive_driver = {
	.config                  = gpio_sifive_config,
	.write                   = gpio_sifive_write,
	.read                    = gpio_sifive_read,
	.port_get_raw            = gpio_sifive_port_get_raw,
	.port_set_masked_raw     = gpio_sifive_port_set_masked_raw,
	.port_set_bits_raw       = gpio_sifive_port_set_bits_raw,
	.port_clear_bits_raw     = gpio_sifive_port_clear_bits_raw,
	.port_toggle_bits        = gpio_sifive_port_toggle_bits,
	.pin_interrupt_configure = gpio_sifive_pin_interrupt_configure,
	.manage_callback         = gpio_sifive_manage_callback,
	.enable_callback         = gpio_sifive_enable_callback,
	.disable_callback        = gpio_sifive_disable_callback,
};

/**
 * @brief Initialize a GPIO controller
 *
 * Perform basic initialization of a GPIO controller
 *
 * @param dev GPIO device struct
 *
 * @return 0
 */
static int gpio_sifive_init(struct device *dev)
{
	volatile struct gpio_sifive_t *gpio = DEV_GPIO(dev);
	const struct gpio_sifive_config *cfg = DEV_GPIO_CFG(dev);

	/* Ensure that all gpio registers are reset to 0 initially */
	gpio->in_en   = 0U;
	gpio->out_en  = 0U;
	gpio->pue     = 0U;
	gpio->rise_ie = 0U;
	gpio->fall_ie = 0U;
	gpio->high_ie = 0U;
	gpio->low_ie  = 0U;
	gpio->invert  = 0U;

	/* Setup IRQ handler for each gpio pin */
	cfg->gpio_cfg_func();

	printk("Initializing gpio,sifive0\n");
	printk("gpio_irq_base: %d\n", cfg->gpio_irq_base);

	return 0;
}

static void gpio_sifive_cfg_0(void);

/*
 * The global IRQ number is the sum of the level 1 and 2 IRQ numbers
 */
#define LEVEL2_IRQ_TO_GLOBAL(irq) \
	((irq >> 8) + (irq & 0xFF))

static const struct gpio_sifive_config gpio_sifive_config0 = {
	.gpio_base_addr    = DT_INST_0_SIFIVE_GPIO0_BASE_ADDRESS,
	/*
	 * This driver assumes that the GPIO interrupts are Level 2 interrupts
	 * managed by a PLIC. Therefore, the global IRQ number for the first
	 * GPIO pin is the sum of the first and second bytes of
	 * DT_INST_0_SIFIVE_GPIO0_IRQ_0.
	 */
	.plic_irq          = (DT_INST_0_SIFIVE_GPIO0_IRQ_0 & 0xFF),
	.gpio_irq_base     = LEVEL2_IRQ_TO_GLOBAL(DT_INST_0_SIFIVE_GPIO0_IRQ_0),
	.gpio_cfg_func     = gpio_sifive_cfg_0,
};

static struct gpio_sifive_data gpio_sifive_data0;

DEVICE_AND_API_INIT(gpio_sifive_0, DT_INST_0_SIFIVE_GPIO0_LABEL,
		    gpio_sifive_init,
		    &gpio_sifive_data0, &gpio_sifive_config0,
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &gpio_sifive_driver);

#define		IRQ_INIT(n)					\
IRQ_CONNECT(DT_INST_0_SIFIVE_GPIO0_IRQ_##n,	\
		CONFIG_GPIO_SIFIVE_##n##_PRIORITY,		\
		gpio_sifive_irq_handler,			\
		DEVICE_GET(gpio_sifive_0),			\
		0);

static void gpio_sifive_cfg_0(void)
{
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_0
	IRQ_INIT(0);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_1
	IRQ_INIT(1);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_2
	IRQ_INIT(2);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_3
	IRQ_INIT(3);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_4
	IRQ_INIT(4);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_5
	IRQ_INIT(5);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_6
	IRQ_INIT(6);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_7
	IRQ_INIT(7);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_8
	IRQ_INIT(8);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_9
	IRQ_INIT(9);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_10
	IRQ_INIT(10);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_11
	IRQ_INIT(11);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_12
	IRQ_INIT(12);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_13
	IRQ_INIT(13);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_14
	IRQ_INIT(14);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_15
	IRQ_INIT(15);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_16
	IRQ_INIT(16);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_17
	IRQ_INIT(17);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_18
	IRQ_INIT(18);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_19
	IRQ_INIT(19);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_20
	IRQ_INIT(20);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_21
	IRQ_INIT(21);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_22
	IRQ_INIT(22);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_23
	IRQ_INIT(23);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_24
	IRQ_INIT(24);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_25
	IRQ_INIT(25);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_26
	IRQ_INIT(26);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_27
	IRQ_INIT(27);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_28
	IRQ_INIT(28);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_29
	IRQ_INIT(29);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_30
	IRQ_INIT(30);
#endif
#ifdef DT_INST_0_SIFIVE_GPIO0_IRQ_31
	IRQ_INIT(31);
#endif
}
