/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MTK_CCCI_COMMON_STUB_H
#define _MTK_CCCI_COMMON_STUB_H

#include <linux/types.h>

enum MD_SYS {
	MD_SYS1 = 0,
};

enum SMEM_USER_ID {
	SMEM_USER_RAW_MD_CONSYS = 22,
};

enum MD_STATE_FOR_USER {
	MD_STATE_INVALID = 0,
	MD_STATE_BOOTING = 1,
	MD_STATE_READY = 2,
	MD_STATE_EXCEPTION = 3,
};

static inline phys_addr_t get_smem_phy_start_addr(int md_id,
		enum SMEM_USER_ID user_id, int *size)
{
	if (size)
		*size = 0;
	return 0;
}

static inline void __iomem *get_smem_start_addr(int md_id,
		enum SMEM_USER_ID user_id, int *size)
{
	if (size)
		*size = 0;
	return NULL;
}

static inline int mtk_ccci_register_md_state_cb(
		void (*cb)(enum MD_STATE_FOR_USER, enum MD_STATE_FOR_USER))
{
	return 0;
}

#endif
