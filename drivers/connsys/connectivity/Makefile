# SPDX-License-Identifier: GPL-2.0
subdir-ccflags-y += $(CONNSYS_CCFLAGS)
ccflags-y += $(CONNSYS_CCFLAGS) -I$(srctree) -I$(srctree)/drivers/mmc/core

MODULE_NAME := connadp
obj-$(CONFIG_MTK_COMBO) += $(MODULE_NAME).o
$(MODULE_NAME)-objs += common/connectivity_build_in_adapter.o
$(MODULE_NAME)-objs += common/wmt_build_in_adapter.o
$(MODULE_NAME)-objs += power_throttling/adapter.o
$(MODULE_NAME)-objs += power_throttling/core.o

obj-$(CONFIG_MTK_COMBO)      += common/
obj-$(CONFIG_MTK_COMBO)      += bt/
obj-$(CONFIG_MTK_COMBO_WIFI) += wlan/adaptor/
obj-$(CONFIG_MTK_COMBO_WIFI) += wlan/core/gen4m/
