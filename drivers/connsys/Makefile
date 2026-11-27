# SPDX-License-Identifier: GPL-2.0
# Out-of-tree MediaTek MT6789 CONNSYS Wi-Fi modules (Advan X1 mainline 7.4)

export CONNSYS_ROOT := $(abspath $(src))
export CONFIG_MTK_PLATFORM := mt6789
export MTK_PLATFORM := mt6789
export CONFIG_MTK_BTIF := m
export CONFIG_MTK_COMBO := m
export CONFIG_MTK_COMBO_WIFI := m
export CONFIG_MTK_COMBO_CHIP_CONSYS_6789 := y
export CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH := y
export CONFIG_MTK_PMIC_CHIP_MT6358 := y
export CONFIG_MTK_COMBO_WIFI_HIF := axi
export MTK_COMBO_CHIP := SOC2_1X1
export BT_PLATFORM := connac1x
export WLAN_CHIP_ID := 6789
export MTK_ANDROID_WMT := y
export MTK_ANDROID_EMI := y
export WIFI_IP_SET := 1
export MTK_WLAN_SERVICE := yes

CONNSYS_CCFLAGS := \
	-I$(CONNSYS_ROOT)/include \
	-I$(CONNSYS_ROOT)/include/mt-plat \
	-I$(CONNSYS_ROOT)/btif/common/inc \
	-I$(CONNSYS_ROOT)/connectivity/common \
	-I$(CONNSYS_ROOT)/connectivity/common/common_main/include \
	-I$(CONNSYS_ROOT)/connectivity/common/common_main/linux/include \
	-I$(CONNSYS_ROOT)/connectivity/common/debug_utility \
	-I$(CONNSYS_ROOT)/connectivity/power_throttling \
	-include $(CONNSYS_ROOT)/include/linux/connsys_compat.h \
	-Wno-error

export CONNSYS_CCFLAGS
subdir-ccflags-y += $(CONNSYS_CCFLAGS)
ccflags-y += $(CONNSYS_CCFLAGS)

obj-$(CONFIG_MTK_BTIF)  += btif/
obj-$(CONFIG_MTK_COMBO) += connectivity/
