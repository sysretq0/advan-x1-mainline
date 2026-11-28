// SPDX-License-Identifier: GPL-2.0
/*
 * hci_mtk_stp.c — Native Linux BlueZ HCI transport over MediaTek CONNSYS STP/WMT.
 *
 * Bridges Linux's standard Bluetooth subsystem (hci0 / BlueZ) directly onto
 * wmt_drv's exported STP BT channel (BT_TASK_INDX) and WMT power control
 * (mtk_wcn_wmt_func_on/off(WMTDRV_TYPE_BT)), avoiding Android's proprietary
 * /dev/stpbt HAL stack.
 *
 * Registration is gated by the writable module parameter:
 *   /sys/module/hci_mtk_stp/parameters/enable
 * so mtk-wmt-init can register hci0 strictly after CONNSYS firmware init.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "osal_typedef.h"
#include "stp_exp.h"
#include "wmt_exp.h"

#define MTK_BT_RX_CHUNK_SZ	2048
#define MTK_BT_TX_RETRIES	8
#define MTK_HCI_OP_SET_BDADDR	0xfc1a

struct hci_mtk_stp {
	struct hci_dev		*hdev;
	struct workqueue_struct	*rx_wq;
	struct work_struct	rx_work;
	struct mutex		tx_lock;
	struct mutex		rx_lock;
	struct sk_buff		*rx_skb;
	u8			rx_chunk[MTK_BT_RX_CHUNK_SZ];
	size_t			rx_need;
	bool			rx_have_hdr;
	bool			opened;
	bool			resetting;
};

static DEFINE_MUTEX(g_mtk_bt_lock);
static struct hci_mtk_stp *g_mtk_bt;
static bool g_mtk_bt_enabled;
static char g_mtk_bt_bdaddr[24];

module_param_string(bdaddr, g_mtk_bt_bdaddr, sizeof(g_mtk_bt_bdaddr), 0644);
MODULE_PARM_DESC(bdaddr, "Optional factory Bluetooth MAC (XX:XX:XX:XX:XX:XX) programmed in hdev->setup");

static size_t h4_hdr_len(u8 pkt_type)
{
	switch (pkt_type) {
	case HCI_EVENT_PKT:
		return HCI_EVENT_HDR_SIZE;
	case HCI_ACLDATA_PKT:
		return HCI_ACL_HDR_SIZE;
	case HCI_SCODATA_PKT:
		return HCI_SCO_HDR_SIZE;
	case HCI_ISODATA_PKT:
		return HCI_ISO_HDR_SIZE;
	default:
		return 0;
	}
}

static size_t h4_payload_len(u8 pkt_type, const u8 *hdr)
{
	switch (pkt_type) {
	case HCI_EVENT_PKT:
		return hdr[1];
	case HCI_ACLDATA_PKT:
		return get_unaligned_le16(hdr + 2);
	case HCI_SCODATA_PKT:
		return hdr[2];
	case HCI_ISODATA_PKT:
		return get_unaligned_le16(hdr + 2) & 0x3fff;
	default:
		return 0;
	}
}

static void hci_mtk_stp_reset_rx_state(struct hci_mtk_stp *bt)
{
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	bt->rx_need = 0;
	bt->rx_have_hdr = false;
}

static void hci_mtk_stp_feed_h4(struct hci_mtk_stp *bt, const u8 *data, size_t len)
{
	struct hci_dev *hdev = bt->hdev;

	while (len > 0) {
		if (!bt->rx_skb) {
			u8 pkt_type = data[0];
			size_t hdr_sz = h4_hdr_len(pkt_type);

			data++;
			len--;
			if (!hdr_sz) {
				bt_dev_warn_ratelimited(hdev, "dropping unknown H4 pkt type 0x%02x",
							pkt_type);
				hdev->stat.err_rx++;
				continue;
			}
			bt->rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_KERNEL);
			if (!bt->rx_skb) {
				hdev->stat.err_rx++;
				return;
			}
			hci_skb_pkt_type(bt->rx_skb) = pkt_type;
			bt->rx_need = hdr_sz;
			bt->rx_have_hdr = false;
		}

		{
			size_t take = min(len, bt->rx_need);

			if (skb_tailroom(bt->rx_skb) < take) {
				bt_dev_err(hdev, "H4 frame overflow (%u + %zu)",
					   bt->rx_skb->len, take);
				hdev->stat.err_rx++;
				hci_mtk_stp_reset_rx_state(bt);
				data += take;
				len -= take;
				continue;
			}
			skb_put_data(bt->rx_skb, data, take);
			data += take;
			len -= take;
			bt->rx_need -= take;
		}

		if (bt->rx_need == 0) {
			if (!bt->rx_have_hdr) {
				u8 pkt_type = hci_skb_pkt_type(bt->rx_skb);
				size_t plen = h4_payload_len(pkt_type, bt->rx_skb->data);

				bt->rx_have_hdr = true;
				bt->rx_need = plen;
				if (plen > skb_tailroom(bt->rx_skb)) {
					bt_dev_err(hdev, "H4 payload too large (%zu)", plen);
					hdev->stat.err_rx++;
					hci_mtk_stp_reset_rx_state(bt);
					continue;
				}
			}
			if (bt->rx_need == 0) {
				struct sk_buff *skb = bt->rx_skb;

				bt->rx_skb = NULL;
				bt->rx_have_hdr = false;
				hdev->stat.byte_rx += skb->len;
				hci_recv_frame(hdev, skb);
			}
		}
	}
}

static void hci_mtk_stp_rx_work(struct work_struct *work)
{
	struct hci_mtk_stp *bt = container_of(work, struct hci_mtk_stp, rx_work);
	INT32 n;

	mutex_lock(&bt->rx_lock);
	while (bt->opened && !bt->resetting) {
		n = mtk_wcn_stp_receive_data(bt->rx_chunk, sizeof(bt->rx_chunk),
					     BT_TASK_INDX);
		if (n <= 0)
			break;
		hci_mtk_stp_feed_h4(bt, bt->rx_chunk, (size_t)n);
	}
	mutex_unlock(&bt->rx_lock);
}

static VOID hci_mtk_stp_event_cb(VOID)
{
	struct hci_mtk_stp *bt = READ_ONCE(g_mtk_bt);

	if (bt && bt->opened && bt->rx_wq)
		queue_work(bt->rx_wq, &bt->rx_work);
}

static VOID hci_mtk_stp_rst_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			       ENUM_WMTMSG_TYPE_T type, PVOID buf, UINT32 sz)
{
	struct hci_mtk_stp *bt = READ_ONCE(g_mtk_bt);
	ENUM_WMTRSTMSG_TYPE_T rst_msg;

	if (!bt || !bt->hdev || sz < sizeof(rst_msg))
		return;
	if (src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_BT ||
	    type != WMTMSG_TYPE_RESET)
		return;

	memcpy(&rst_msg, buf, sizeof(rst_msg));
	if (rst_msg == WMTRSTMSG_RESET_START) {
		bt_dev_warn(bt->hdev, "CONNSYS whole-chip reset started");
		bt->resetting = true;
	} else if (rst_msg == WMTRSTMSG_RESET_END ||
		   rst_msg == WMTRSTMSG_RESET_END_FAIL) {
		struct sk_buff *skb;
		static const u8 hw_err[3] = { HCI_EV_HARDWARE_ERROR, 0x01, 0x00 };

		bt_dev_warn(bt->hdev, "CONNSYS whole-chip reset finished (%d)", rst_msg);
		bt->resetting = false;
		skb = bt_skb_alloc(sizeof(hw_err), GFP_ATOMIC);
		if (skb) {
			hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
			skb_put_data(skb, hw_err, sizeof(hw_err));
			hci_recv_frame(bt->hdev, skb);
		}
	}
}

static int hci_mtk_stp_open(struct hci_dev *hdev)
{
	struct hci_mtk_stp *bt = hci_get_drvdata(hdev);

	if (bt->opened)
		return 0;

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "mtk_wcn_wmt_func_on(BT) failed");
		return -EIO;
	}
	if (mtk_wcn_stp_is_ready() == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "CONNSYS STP is not ready");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		return -EIO;
	}

	mtk_wcn_stp_set_bluez(MTK_WCN_BOOL_FALSE);
	bt->resetting = false;
	bt->opened = true;
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, hci_mtk_stp_event_cb);
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, hci_mtk_stp_rst_cb);

	if (!mtk_wcn_stp_is_rxqueue_empty(BT_TASK_INDX))
		queue_work(bt->rx_wq, &bt->rx_work);

	bt_dev_info(hdev, "MTK CONNSYS STP Bluetooth transport opened");
	return 0;
}

static int hci_mtk_stp_close(struct hci_dev *hdev)
{
	struct hci_mtk_stp *bt = hci_get_drvdata(hdev);

	if (!bt->opened)
		return 0;

	bt->opened = false;
	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	cancel_work_sync(&bt->rx_work);

	mutex_lock(&bt->rx_lock);
	hci_mtk_stp_reset_rx_state(bt);
	mutex_unlock(&bt->rx_lock);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE)
		bt_dev_warn(hdev, "mtk_wcn_wmt_func_off(BT) failed");

	bt_dev_info(hdev, "MTK CONNSYS STP Bluetooth transport closed");
	return 0;
}

static int hci_mtk_stp_flush(struct hci_dev *hdev)
{
	struct hci_mtk_stp *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->rx_lock);
	hci_mtk_stp_reset_rx_state(bt);
	mutex_unlock(&bt->rx_lock);
	return 0;
}

static int hci_mtk_stp_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_mtk_stp *bt = hci_get_drvdata(hdev);
	u8 pkt_type = hci_skb_pkt_type(skb);
	INT32 sent = 0;
	int retry;

	if (!bt->opened || bt->resetting) {
		kfree_skb(skb);
		return -ENODEV;
	}

	switch (pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	case HCI_ISODATA_PKT:
		break;
	default:
		kfree_skb(skb);
		return -EILSEQ;
	}

	memcpy(skb_push(skb, 1), &pkt_type, 1);

	mutex_lock(&bt->tx_lock);
	for (retry = 0; retry < MTK_BT_TX_RETRIES; retry++) {
		sent = mtk_wcn_stp_send_data(skb->data, skb->len, BT_TASK_INDX);
		if (sent > 0 || sent < 0)
			break;
		msleep(10);
	}
	mutex_unlock(&bt->tx_lock);

	if (sent != (INT32)skb->len) {
		bt_dev_err(hdev, "stp_send_data failed (len=%u, sent=%d)",
			   skb->len, sent);
		hdev->stat.err_tx++;
		kfree_skb(skb);
		return (sent < 0) ? (int)sent : -EIO;
	}

	hdev->stat.byte_tx += skb->len;
	kfree_skb(skb);
	return 0;
}

static int hci_mtk_stp_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;
	int err;

	skb = __hci_cmd_sync(hdev, MTK_HCI_OP_SET_BDADDR, 6, bdaddr, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "changing MediaTek BD_ADDR failed (%d)", err);
		return err;
	}
	kfree_skb(skb);
	return 0;
}

static bool hci_mtk_stp_parse_bdaddr(const char *str, bdaddr_t *out)
{
	unsigned int b[6];

	if (!str || !str[0])
		return false;
	if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
		   &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6)
		return false;
	if ((b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) == 0 ||
	    (b[0] & b[1] & b[2] & b[3] & b[4] & b[5]) == 0xff)
		return false;
	for (int i = 0; i < 6; i++)
		out->b[i] = (u8)b[i];
	return true;
}

static int hci_mtk_stp_setup(struct hci_dev *hdev)
{
	bdaddr_t bdaddr;

	if (hci_mtk_stp_parse_bdaddr(g_mtk_bt_bdaddr, &bdaddr)) {
		int err = hci_mtk_stp_set_bdaddr(hdev, &bdaddr);

		if (err)
			bt_dev_warn(hdev, "factory BD_ADDR setup ignored (%d)", err);
		else
			bt_dev_info(hdev, "programmed factory BD_ADDR %pMR", &bdaddr);
	}
	return 0;
}

static int hci_mtk_stp_register(void)
{
	struct hci_mtk_stp *bt;
	struct hci_dev *hdev;
	int err;

	if (g_mtk_bt)
		return 0;

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	mutex_init(&bt->tx_lock);
	mutex_init(&bt->rx_lock);
	INIT_WORK(&bt->rx_work, hci_mtk_stp_rx_work);
	bt->rx_wq = alloc_ordered_workqueue("hci_mtk_stp_rx", WQ_HIGHPRI);
	if (!bt->rx_wq) {
		kfree(bt);
		return -ENOMEM;
	}

	hdev = hci_alloc_dev();
	if (!hdev) {
		destroy_workqueue(bt->rx_wq);
		kfree(bt);
		return -ENOMEM;
	}

	bt->hdev = hdev;
	hdev->bus = HCI_SMD;
	hci_set_drvdata(hdev, bt);

	hdev->open = hci_mtk_stp_open;
	hdev->close = hci_mtk_stp_close;
	hdev->flush = hci_mtk_stp_flush;
	hdev->send = hci_mtk_stp_send_frame;
	hdev->setup = hci_mtk_stp_setup;
	hdev->set_bdaddr = hci_mtk_stp_set_bdaddr;
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	/*
	 * MT6789/MT6631 CONNSYS ("MTK SOC2_2 #1") advertises LMP_SYNC_TRAIN
	 * (bit 2) in Extended Features Page 2, but rejects HCI_OP_READ_SYNC_TRAIN_PARAMS
	 * (0x0c77) with HCI 0x01 (Unknown Command / -EBADRQC), aborting hci_init4.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2);

	WRITE_ONCE(g_mtk_bt, bt);
	err = hci_register_dev(hdev);
	if (err) {
		WRITE_ONCE(g_mtk_bt, NULL);
		hci_free_dev(hdev);
		destroy_workqueue(bt->rx_wq);
		kfree(bt);
		pr_err("hci_mtk_stp: hci_register_dev failed (%d)\n", err);
		return err;
	}

	pr_info("hci_mtk_stp: registered %s over CONNSYS STP/WMT\n", hdev->name);
	return 0;
}

static void hci_mtk_stp_unregister(void)
{
	struct hci_mtk_stp *bt = g_mtk_bt;

	if (!bt)
		return;

	hci_unregister_dev(bt->hdev);
	WRITE_ONCE(g_mtk_bt, NULL);
	hci_free_dev(bt->hdev);
	destroy_workqueue(bt->rx_wq);
	hci_mtk_stp_reset_rx_state(bt);
	kfree(bt);
	pr_info("hci_mtk_stp: unregistered CONNSYS STP Bluetooth device\n");
}

static int hci_mtk_stp_enable_set(const char *val, const struct kernel_param *kp)
{
	bool want = false;
	int err = kstrtobool(val, &want);

	if (err)
		return err;

	mutex_lock(&g_mtk_bt_lock);
	if (want)
		err = hci_mtk_stp_register();
	else
		hci_mtk_stp_unregister();
	if (!err)
		g_mtk_bt_enabled = want;
	mutex_unlock(&g_mtk_bt_lock);
	return err;
}

static const struct kernel_param_ops hci_mtk_stp_enable_ops = {
	.set = hci_mtk_stp_enable_set,
	.get = param_get_bool,
};
module_param_cb(enable, &hci_mtk_stp_enable_ops, &g_mtk_bt_enabled, 0644);
MODULE_PARM_DESC(enable, "Register (1) or unregister (0) native BlueZ hci0 over CONNSYS STP/WMT");

static int __init hci_mtk_stp_init(void)
{
	pr_info("hci_mtk_stp: loaded (write 1 to /sys/module/hci_mtk_stp/parameters/enable after WMT init)\n");
	return 0;
}

static void __exit hci_mtk_stp_exit(void)
{
	mutex_lock(&g_mtk_bt_lock);
	hci_mtk_stp_unregister();
	g_mtk_bt_enabled = false;
	mutex_unlock(&g_mtk_bt_lock);
}

module_init(hci_mtk_stp_init);
module_exit(hci_mtk_stp_exit);

MODULE_AUTHOR("Advan X1 Mainline Contributors");
MODULE_DESCRIPTION("MediaTek MT6789 CONNSYS STP/WMT Native Linux Bluetooth HCI Transport");
MODULE_LICENSE("GPL");
