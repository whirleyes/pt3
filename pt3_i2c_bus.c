#include "version.h"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/sched.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
#include <asm/system.h>
#endif
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

#include "pt3_com.h"
#include "pt3_pci.h"
#include "pt3_i2c_bus.h"

#define MAX_INSTRUCTIONS 4096

static __u32 tmp_inst;

typedef struct _PT3_I2C_BUS_PRIV_DATA {
	__u8 *sbuf;
} PT3_I2C_BUS_PRIV_DATA;

enum {
	I_END,
	I_ADDRESS,
	I_CLOCK_L,
	I_CLOCK_H,
	I_DATA_L,
	I_DATA_H,
	I_RESET,
	I_SLEEP,	// Sleep 1ms
	I_DATA_L_NOP  = 0x08,
	I_DATA_H_NOP  = 0x0c,
	I_DATA_H_READ = 0x0d,
	I_DATA_H_ACK0 = 0x0e,
	I_DATA_H_ACK1 = 0x0f,

	// テスト用
	_I_DATA_L_READ = I_DATA_H_READ ^ 0x04
//	_I_DATA_L_ACK0 = I_DATA_H_ACK0 ^ 0x04,
//	_I_DATA_L_ACK1 = I_DATA_H_ACK1 ^ 0x04
};

static void
add_instruction(PT3_I2C_BUS *bus, __u32 instruction)
{
	__u8 *dst;
	PT3_I2C_BUS_PRIV_DATA *priv;
	priv = bus->priv;

	if ((bus->inst_count % 2) == 0) {
		tmp_inst = instruction;
	} else {
		tmp_inst |= instruction << 4;
	}

	if (bus->inst_count % 2) {
		dst = priv->sbuf + (bus->inst_count / 2);
		memcpy(dst, &tmp_inst, sizeof(tmp_inst));
	}
	bus->inst_count += 1;
}

void
pt3_i2c_bus_start(PT3_I2C_BUS *bus)
{
	add_instruction(bus, I_DATA_H);
	add_instruction(bus, I_CLOCK_H);
	add_instruction(bus, I_DATA_L);
	add_instruction(bus, I_CLOCK_L);
}

void
pt3_i2c_bus_stop(PT3_I2C_BUS *bus)
{
	//add_instruction(bus, I_CLOCK_L);
	add_instruction(bus, I_DATA_L);
	add_instruction(bus, I_CLOCK_H);
	add_instruction(bus, I_DATA_H);
}

void
pt3_i2c_bus_write(PT3_I2C_BUS *bus, __u8 *data, __u32 size)
{
	__u32 i, j;
	__u8 byte;

	for (i = 0; i < size; i++) {
		byte = data[i];
		for (j = 0; j < 8; j++) {
			add_instruction(bus, BIT_SHIFT_MASK(byte, 7 - j, 1) ?
									I_DATA_H_NOP : I_DATA_L_NOP);
		}
		add_instruction(bus, I_DATA_H_ACK0);
	}
}

/* FPGA_I2C */

static void
wait(PT3_I2C_BUS *bus, __u32 *data)
{
	__u32 val;
	
	while (1) {
		val = readl(bus->base + REGS_I2C_R);
		if (!BIT_SHIFT_MASK(val, 0, 1))
			break;
		schedule_timeout_interruptible(msecs_to_jiffies(1));	
	}

	if (data != NULL)
		*data = val;
}

static int
run_code(PT3_I2C_BUS *bus, __u32 start_addr, __u32 *ack)
{
	__u32 data;

	wait(bus, &data);

	writel(1 << 16 | start_addr, bus->base + REGS_I2C_W);

	wait(bus, &data);

	data = BIT_SHIFT_MASK(data, 1, 2);
	if (ack != NULL)
		*ack = data;

	return data ? -1 : 0;
}

void
pt3_i2c_bus_copy(PT3_I2C_BUS *bus)
{
	__u8 *src;
	__u8 *dst;
	PT3_I2C_BUS_PRIV_DATA *priv;
	priv = bus->priv;

	src = priv->sbuf;
	dst = bus->base + REGS_I2C_INST + (bus->inst_addr / 2);
	memcpy(dst, src, (bus->inst_count / 2));
}

int
pt3_i2c_bus_run(PT3_I2C_BUS *bus, __u32 *ack, int copy)
{
	int ret;

	mutex_lock(&bus->lock);

	if (copy) {
		pt3_i2c_bus_copy(bus);
	}

	ret = run_code(bus, bus->inst_addr, ack);

	mutex_unlock(&bus->lock);

	return ret;
}

int
pt3_i2c_bus_is_clean(PT3_I2C_BUS *bus)
{
	__u32 val;

	val = readl(bus->base + REGS_I2C_R);

	return BIT_SHIFT_MASK(val, 3, 1);
}

void
pt3_i2c_bus_reset(PT3_I2C_BUS *bus)
{
	writel(1 << 17, bus->base + REGS_I2C_W);
}

PT3_I2C_BUS *
create_pt3_i2c_bus(void __iomem *regs)
{
	PT3_I2C_BUS *bus;
	PT3_I2C_BUS_PRIV_DATA *priv;
	__u8 *sbuf;

	bus = NULL;
	priv = NULL;
	sbuf = NULL;

	bus = kzalloc(sizeof(PT3_I2C_BUS), GFP_KERNEL);

	if (bus == NULL)
		goto fail;

	priv = kzalloc(sizeof(PT3_I2C_BUS_PRIV_DATA), GFP_KERNEL);
	if (priv == NULL)
		goto fail;

	sbuf = kzalloc(MAX_INSTRUCTIONS / 2, GFP_KERNEL);
	if (sbuf == NULL)
		goto fail;

	mutex_init(&bus->lock);
	bus->base = regs;
	priv->sbuf = sbuf;
	bus->priv = priv;

	return bus;
fail:
	free_pt3_i2c_bus(bus);
	return NULL;
}

void
free_pt3_i2c_bus(PT3_I2C_BUS *bus)
{
	PT3_I2C_BUS_PRIV_DATA *priv;
	priv = bus->priv;

	if (priv != NULL) {
		if (priv->sbuf != NULL)
			kfree(priv->sbuf);
		kfree(priv);
	}

	kfree(bus);
}