/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CONN_MD_EXP_STUB_H
#define _CONN_MD_EXP_STUB_H

#include <linux/types.h>

#define uint32 unsigned int
#define uint8 unsigned char
#define uint16 unsigned short
#ifdef CHAR
#undef CHAR
#endif

struct local_para {
	u8 ref_count;
	u8 _stub;
	u16 msg_len;
	u8 data[];
};

struct ipc_ilm {
	u32 src_mod_id;
	u32 dest_mod_id;
	u32 sap_id;
	u32 msg_id;
	struct local_para *local_para_ptr;
	void *peer_buff_ptr;
};

#define conn_md_ipc_ilm_t struct ipc_ilm

enum CONN_MD_ERR_CODE {
	CONN_MD_ERR_NO_ERR = 0,
	CONN_MD_ERR_DEF_ERR = -1,
	CONN_MD_ERR_INVALID_PARAM = -2,
	CONN_MD_ERR_OTHERS = -4,
};

typedef int (*conn_md_msg_rx_cb)(struct ipc_ilm *ilm);

struct conn_md_bridge_ops {
	conn_md_msg_rx_cb rx_cb;
};

static inline int mtk_conn_md_bridge_reg(uint32 u_id,
					 struct conn_md_bridge_ops *p_ops)
{
	return 0;
}

static inline int mtk_conn_md_bridge_unreg(uint32 u_id)
{
	return 0;
}

static inline int mtk_conn_md_bridge_send_msg(struct ipc_ilm *ilm)
{
	return 0;
}

#endif
