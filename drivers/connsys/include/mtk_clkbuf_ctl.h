/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MTK_CLKBUF_CTL_STUB_H
#define _MTK_CLKBUF_CTL_STUB_H
#include <linux/types.h>
enum clk_buf_id {
	CLK_BUF_BB_MD = 0,
	CLK_BUF_CONN,
	CLK_BUF_NFC,
	CLK_BUF_RF,
	CLK_BUF_UFS = 6,
	CLK_BUF_INVALID
};
static inline bool clk_buf_ctrl(enum clk_buf_id id, bool onoff)
{
	return true;
}
#endif
