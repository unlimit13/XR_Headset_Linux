/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Copyright 2017~2018,2020 NXP
 *
 * Header file containing the public System Controller Interface (SCI)
 * definitions.
 */

#ifndef _SC_SCI_H
#define _SC_SCI_H

#include <linux/firmware/imx/ipc.h>

#include <linux/firmware/imx/svc/misc.h>
#include <linux/firmware/imx/svc/pm.h>
#include <linux/firmware/imx/svc/rm.h>
#include <linux/firmware/imx/svc/seco.h>

#define IMX_SC_IRQ_GROUP_WAKE       3U /* Wakeup interrupts */
#define IMX_SC_IRQ_SECVIO            BIT(6)    /* Security violation */

int imx_scu_enable_general_irq_channel(struct device *dev);
int imx_scu_irq_register_notifier(struct notifier_block *nb);
int imx_scu_irq_unregister_notifier(struct notifier_block *nb);
int imx_scu_irq_group_enable(u8 group, u32 mask, u8 enable);
int imx_scu_irq_get_status(u8 group, u32 *irq_status);
#endif /* _SC_SCI_H */
