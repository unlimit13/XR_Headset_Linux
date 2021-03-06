// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018 NXP
 *  Author: Dong Aisheng <aisheng.dong@nxp.com>
 *
 * Implementation of the SCU IPC functions using MUs (client side).
 *
 */

#include <linux/arm-smccc.h>
#include <linux/err.h>
#include <linux/firmware/imx/ipc.h>
#include <linux/firmware/imx/sci.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <xen/xen.h>

#define FSL_HVC_SC                      0xC6000000
#define SCU_MU_CHAN_NUM		8
#define MAX_RX_TIMEOUT		(msecs_to_jiffies(3000))

struct imx_sc_chan {
	struct imx_sc_ipc *sc_ipc;

	struct mbox_client cl;
	struct mbox_chan *ch;
	int idx;
	struct completion tx_done;
	u8 rx_pos;
};

struct imx_sc_ipc {
	/* SCU uses 4 Tx and 4 Rx channels */
	struct imx_sc_chan chans[SCU_MU_CHAN_NUM];
	struct device *dev;
	struct mutex lock;
	struct completion done;

	/* temporarily store the SCU msg */
	u32 *msg;
	u8 rx_size;
	u8 count;
};

/*
 * This type is used to indicate error response for most functions.
 */
enum imx_sc_error_codes {
	IMX_SC_ERR_NONE = 0,	/* Success */
	IMX_SC_ERR_VERSION = 1,	/* Incompatible API version */
	IMX_SC_ERR_CONFIG = 2,	/* Configuration error */
	IMX_SC_ERR_PARM = 3,	/* Bad parameter */
	IMX_SC_ERR_NOACCESS = 4,	/* Permission error (no access) */
	IMX_SC_ERR_LOCKED = 5,	/* Permission error (locked) */
	IMX_SC_ERR_UNAVAILABLE = 6,	/* Unavailable (out of resources) */
	IMX_SC_ERR_NOTFOUND = 7,	/* Not found */
	IMX_SC_ERR_NOPOWER = 8,	/* No power */
	IMX_SC_ERR_IPC = 9,		/* Generic IPC error */
	IMX_SC_ERR_BUSY = 10,	/* Resource is currently busy/active */
	IMX_SC_ERR_FAIL = 11,	/* General I/O failure */
	IMX_SC_ERR_LAST
};

static int imx_sc_linux_errmap[IMX_SC_ERR_LAST] = {
	0,	 /* IMX_SC_ERR_NONE */
	-EINVAL, /* IMX_SC_ERR_VERSION */
	-EINVAL, /* IMX_SC_ERR_CONFIG */
	-EINVAL, /* IMX_SC_ERR_PARM */
	-EACCES, /* IMX_SC_ERR_NOACCESS */
	-EACCES, /* IMX_SC_ERR_LOCKED */
	-ERANGE, /* IMX_SC_ERR_UNAVAILABLE */
	-EEXIST, /* IMX_SC_ERR_NOTFOUND */
	-EPERM,	 /* IMX_SC_ERR_NOPOWER */
	-EPIPE,	 /* IMX_SC_ERR_IPC */
	-EBUSY,	 /* IMX_SC_ERR_BUSY */
	-EIO,	 /* IMX_SC_ERR_FAIL */
};

static struct imx_sc_ipc *imx_sc_ipc_handle;

static inline int imx_sc_to_linux_errno(int errno)
{
	if (errno >= IMX_SC_ERR_NONE && errno < IMX_SC_ERR_LAST)
		return imx_sc_linux_errmap[errno];
	return -EIO;
}

/*
 * Get the default handle used by SCU
 */
int imx_scu_get_handle(struct imx_sc_ipc **ipc)
{
	if (!imx_sc_ipc_handle)
		return -EPROBE_DEFER;

	*ipc = imx_sc_ipc_handle;
	return 0;
}
EXPORT_SYMBOL(imx_scu_get_handle);

/* Callback called when the word of a message is ack-ed, eg read by SCU */
static void imx_scu_tx_done(struct mbox_client *cl, void *mssg, int r)
{
	struct imx_sc_chan *sc_chan = container_of(cl, struct imx_sc_chan, cl);

	complete(&sc_chan->tx_done);
}

static void imx_scu_rx_callback(struct mbox_client *c, void *msg)
{
	struct imx_sc_chan *sc_chan = container_of(c, struct imx_sc_chan, cl);
	struct imx_sc_ipc *sc_ipc = sc_chan->sc_ipc;
	struct imx_sc_rpc_msg *hdr;
	u32 *data = msg;

	if (sc_chan->rx_pos == 0) {
		hdr = msg;
		sc_ipc->rx_size = hdr->size;
		dev_dbg(sc_ipc->dev, "msg rx size %u\n", sc_ipc->rx_size);
	}

	sc_ipc->msg[sc_chan->rx_pos] = *data;
	sc_chan->rx_pos += 4;
	sc_ipc->count++;

	dev_dbg(sc_ipc->dev, "mu %u msg %u 0x%x\n", sc_chan->idx,
		sc_ipc->count, *data);

	if (sc_ipc->count == sc_ipc->rx_size)
		complete(&sc_ipc->done);
}

static void imx_scu_big_rx_callback(struct mbox_client *c, void *msg)
{
	struct imx_sc_chan *sc_chan = container_of(c, struct imx_sc_chan, cl);
	struct imx_sc_ipc *sc_ipc = sc_chan->sc_ipc;
	struct imx_sc_rpc_msg *hdr;
	u32 *data = msg;

	if (sc_ipc->count == 0) {
		hdr = msg;
		sc_ipc->rx_size = hdr->size;
		dev_dbg(sc_ipc->dev, "msg rx size %u\n", sc_ipc->rx_size);
	}

	sc_ipc->msg[sc_ipc->count] = *data;
	sc_ipc->count++;

	dev_dbg(sc_ipc->dev, "mu %u msg %u 0x%x\n", sc_chan->idx,
		sc_ipc->count, *data);

	if (sc_ipc->count == sc_ipc->rx_size)
		complete(&sc_ipc->done);
}

static int imx_scu_ipc_write(struct imx_sc_ipc *sc_ipc, void *msg)
{
	struct imx_sc_rpc_msg hdr = *(struct imx_sc_rpc_msg *)msg;
	struct imx_sc_chan *sc_chan;
	u32 *data = msg;
	int ret;
	int i;

	/* Check size */
	if (hdr.size > IMX_SC_RPC_MAX_MSG)
		return -EINVAL;

	dev_dbg(sc_ipc->dev, "RPC SVC %u FUNC %u SIZE %u\n", hdr.svc,
		hdr.func, hdr.size);

	for (i = 0; i < hdr.size; i++) {
		sc_chan = &sc_ipc->chans[i % 4];

		/*
		 * SCU requires that all messages words are written
		 * sequentially but linux MU driver implements multiple
		 * independent channels for each register so ordering between
		 * different channels must be ensured by SCU API interface.
		 *
		 * Wait for tx_done before every send to ensure that no
		 * queueing happens at the mailbox channel level.
		 */
		wait_for_completion(&sc_chan->tx_done);
		reinit_completion(&sc_chan->tx_done);

		ret = mbox_send_message(sc_chan->ch, &data[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * RPC command/response
 */
int imx_scu_call_rpc(struct imx_sc_ipc *sc_ipc, void *msg, bool have_resp)
{
	struct imx_sc_rpc_msg *hdr;
	struct arm_smccc_res res;
	int ret;
	int i;

	if (WARN_ON(!sc_ipc || !msg))
		return -EINVAL;

	mutex_lock(&sc_ipc->lock);

	for (i = 4; i < 8; i++) {
		struct imx_sc_chan *sc_chan = &sc_ipc->chans[i];

		sc_chan->rx_pos = sc_chan->idx;
	}

	reinit_completion(&sc_ipc->done);

	sc_ipc->msg = msg;
	sc_ipc->count = 0;
	sc_ipc->rx_size = 0;

	if (xen_initial_domain()) {
		arm_smccc_hvc(FSL_HVC_SC, (uint64_t)msg, !have_resp, 0, 0, 0,
			      0, 0, &res);
		if (res.a0)
			printk("Error FSL_HVC_SC %ld\n", res.a0);

		ret = res.a0;

	} else {
		ret = imx_scu_ipc_write(sc_ipc, msg);
		if (ret < 0) {
			dev_err(sc_ipc->dev, "RPC send msg failed: %d\n", ret);
			goto out;
		}

		if (have_resp) {
			if (!wait_for_completion_timeout(&sc_ipc->done,
							 MAX_RX_TIMEOUT)) {
				dev_err(sc_ipc->dev, "RPC send msg timeout\n");
				mutex_unlock(&sc_ipc->lock);
				return -ETIMEDOUT;
			}

			/* response status is stored in hdr->func field */
			hdr = msg;
			ret = hdr->func;
		}
	}

out:
	mutex_unlock(&sc_ipc->lock);

	dev_dbg(sc_ipc->dev, "RPC SVC done\n");

	return imx_sc_to_linux_errno(ret);
}
EXPORT_SYMBOL(imx_scu_call_rpc);

int imx_scu_call_big_rpc(struct imx_sc_ipc *sc_ipc, void *msg, bool have_resp)
{
	struct imx_sc_rpc_msg *hdr;
	struct arm_smccc_res res;
	int ret;
	int i;

	if (WARN_ON(!sc_ipc || !msg))
		return -EINVAL;

	mutex_lock(&sc_ipc->lock);
	for (i = 4; i < 8; i++) {
		struct mbox_client *cl = &sc_ipc->chans[i].cl;

		cl->rx_callback = imx_scu_big_rx_callback;
	}

	reinit_completion(&sc_ipc->done);

	sc_ipc->msg = msg;
	sc_ipc->count = 0;
	sc_ipc->rx_size = 0;
	if (xen_initial_domain()) {
		arm_smccc_hvc(FSL_HVC_SC, (uint64_t)msg, !have_resp, 0, 0, 0,
			      0, 0, &res);
		if (res.a0)
			printk("Error FSL_HVC_SC %ld\n", res.a0);

		ret = res.a0;

	} else {
		ret = imx_scu_ipc_write(sc_ipc, msg);
		if (ret < 0) {
			dev_err(sc_ipc->dev, "RPC send msg failed: %d\n", ret);
			goto out;
		}

		if (have_resp) {
			if (!wait_for_completion_timeout(&sc_ipc->done,
							 MAX_RX_TIMEOUT)) {
				dev_err(sc_ipc->dev, "RPC send msg timeout\n");
				mutex_unlock(&sc_ipc->lock);
				return -ETIMEDOUT;
			}

			/* response status is stored in hdr->func field */
			hdr = msg;
			ret = hdr->func;
		}
	}

out:
	for (i = 4; i < 8; i++) {
		struct mbox_client *cl = &sc_ipc->chans[i].cl;

		cl->rx_callback = imx_scu_rx_callback;
	}
	mutex_unlock(&sc_ipc->lock);

	dev_dbg(sc_ipc->dev, "RPC SVC done\n");

	return imx_sc_to_linux_errno(ret);
}
EXPORT_SYMBOL(imx_scu_call_big_rpc);

static int imx_scu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx_sc_ipc *sc_ipc;
	struct imx_sc_chan *sc_chan;
	struct mbox_client *cl;
	char *chan_name;
	int ret;
	int i;

	sc_ipc = devm_kzalloc(dev, sizeof(*sc_ipc), GFP_KERNEL);
	if (!sc_ipc)
		return -ENOMEM;

	for (i = 0; i < SCU_MU_CHAN_NUM; i++) {
		if (i < 4)
			chan_name = kasprintf(GFP_KERNEL, "tx%d", i);
		else
			chan_name = kasprintf(GFP_KERNEL, "rx%d", i - 4);

		if (!chan_name)
			return -ENOMEM;

		sc_chan = &sc_ipc->chans[i];
		cl = &sc_chan->cl;
		cl->dev = dev;
		cl->tx_block = false;
		cl->knows_txdone = true;
		cl->rx_callback = imx_scu_rx_callback;

		/* Initial tx_done completion as "done" */
		cl->tx_done = imx_scu_tx_done;
		init_completion(&sc_chan->tx_done);
		complete(&sc_chan->tx_done);

		sc_chan->sc_ipc = sc_ipc;
		sc_chan->idx = i % 4;
		sc_chan->ch = mbox_request_channel_byname(cl, chan_name);
		if (IS_ERR(sc_chan->ch)) {
			ret = PTR_ERR(sc_chan->ch);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Failed to request mbox chan %s ret %d\n",
					chan_name, ret);
			return ret;
		}

		dev_dbg(dev, "request mbox chan %s\n", chan_name);
		/* chan_name is not used anymore by framework */
		kfree(chan_name);
	}

	sc_ipc->dev = dev;
	mutex_init(&sc_ipc->lock);
	init_completion(&sc_ipc->done);

	imx_sc_ipc_handle = sc_ipc;

	ret = imx_scu_enable_general_irq_channel(dev);
	if (ret)
		dev_warn(dev,
			"failed to enable general irq channel: %d\n", ret);

	dev_info(dev, "NXP i.MX SCU Initialized\n");

	return devm_of_platform_populate(dev);
}

static const struct of_device_id imx_scu_match[] = {
	{ .compatible = "fsl,imx-scu", },
	{ /* Sentinel */ }
};

static struct platform_driver imx_scu_driver = {
	.driver = {
		.name = "imx-scu",
		.of_match_table = imx_scu_match,
	},
	.probe = imx_scu_probe,
};

static int __init imx_scu_driver_init(void)
{
	return platform_driver_register(&imx_scu_driver);
}
subsys_initcall_sync(imx_scu_driver_init);

MODULE_AUTHOR("Dong Aisheng <aisheng.dong@nxp.com>");
MODULE_DESCRIPTION("IMX SCU firmware protocol driver");
MODULE_LICENSE("GPL v2");
