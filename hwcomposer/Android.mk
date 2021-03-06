LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

ifeq ($(MTK_HWC_SUPPORT), yes)

ifneq ($(findstring 2., $(MTK_HWC_VERSION)),)
LOCAL_SRC_FILES := \
	hwc2_api.cpp
else
LOCAL_SRC_FILES := \
	hwc.cpp
endif

LOCAL_CFLAGS := \
	-DLOG_TAG=\"hwcomposer\"

ifeq ($(MTK_HDMI_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_EXTERNAL_SUPPORT
endif

ifeq ($(MTK_WFD_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_VIRTUAL_SUPPORT
endif

ifneq ($(MTK_PQ_SUPPORT), PQ_OFF)
LOCAL_CFLAGS += -DMTK_ENHAHCE_SUPPORT
endif

ifeq ($(MTK_ROTATION_OFFSET_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_ROTATION_OFFSET_SUPPORT
endif

ifeq ($(MTK_GMO_RAM_OPTIMIZE), yes)
LOCAL_CFLAGS += -DMTK_GMO_RAM_OPTIMIZE
endif

ifeq ($(MTK_GLOBAL_PQ_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_GLOBAL_PQ_SUPPORT
endif

ifeq ($(TARGET_FORCE_HWC_FOR_VIRTUAL_DISPLAYS), true)
LOCAL_CFLAGS += -DMTK_FORCE_HWC_COPY_VDS
endif

ifeq ($(TARGET_RUNNING_WITHOUT_SYNC_FRAMEWORK), true)
LOCAL_CFLAGS += -DMTK_WITHOUT_PRIMARY_PRESENT_FENCE
endif

ifneq ($(MTK_BASIC_PACKAGE), yes)
ifneq ($(MTK_ADJUST_VSYNC_OFFSET_ACTIVELY), no)
ifneq ($(MTK_MERGE_MDP_DISPLAY), no)
LOCAL_CFLAGS += -DMTK_MERGE_MDP_DISPLAY
endif
endif
endif

ifneq ($(LINUX_KERNEL_VERSION), kernel-3.10)
LOCAL_CFLAGS += -DMTK_CONTROL_POWER_WITH_FRAMEBUFFER_DEVICE
endif

# 0:MHL/HDMI  1:EPAPER  2:LCD
ifneq ($(MTK_DUAL_DISPLAY), )
LOCAL_CFLAGS += -DMTK_DUAL_DISPLAY=$(MTK_DUAL_DISPLAY)
else
LOCAL_CFLAGS += -DMTK_DUAL_DISPLAY=0
endif

ifeq ($(MTK_DUAL_DISPLAY), 1)
ifneq ($(MTK_EPAPER_VENDOR),)
LOCAL_CFLAGS += -DMTK_EPAPER_VENDOR=\"$(MTK_EPAPER_VENDOR)\"
else
LOCAL_CFLAGS += -DMTK_EPAPER_VENDOR=\"dummy\"
endif
else
LOCAL_CFLAGS += -DMTK_EPAPER_VENDOR=NULL
endif

ifeq ($(MTK_AOD_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_AOD_SUPPORT
endif

ifeq ($(MTK_RESOLUTION_SWITCH_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_RESOLUTION_SWITCH_SUPPORT
endif

ifneq ($(MTK_ADJUST_VSYNC_OFFSET_ACTIVELY), no)
ifneq ($(MTK_MERGE_MDP_DISPLAY), no)
ifneq ($(MTK_BASIC_PACKAGE), yes)
LOCAL_CFLAGS += -DMTK_MERGE_MDP_DISPLAY
endif
endif
endif

ifneq ($(findstring 1.2, $(MTK_HWC_VERSION)),)
LOCAL_CFLAGS += -DMTK_HWC_VER_1_2
endif

ifneq ($(findstring 1.3, $(MTK_HWC_VERSION)),)
LOCAL_CFLAGS += -DMTK_HWC_VER_1_3
endif

ifneq ($(findstring 1.4, $(MTK_HWC_VERSION)),)
LOCAL_CFLAGS += -DMTK_HWC_VER_1_4
endif

ifneq ($(findstring 1.5, $(MTK_HWC_VERSION)),)
LOCAL_CFLAGS += -DMTK_HWC_VER_1_5
endif

ifneq ($(findstring 2.0, $(MTK_HWC_VERSION)),)
LOCAL_CFLAGS += -DMTK_HWC_VER_2_0
LOCAL_CFLAGS += -USE_HWC2
endif



LOCAL_C_INCLUDES += \
	$(TOP)/$(MTK_ROOT)/hardware/hwcomposer/include \
	#$(TOP)/$(MTK_ROOT)/hardware/include

LOCAL_STATIC_LIBRARIES += \
	hwcomposer.$(MTK_PLATFORM_DIR).$(MTK_HWC_VERSION)

LOCAL_SHARED_LIBRARIES := \
	libui \
	libutils \
	libcutils \
        liblog \
	libsync \
	libion \
	libbwc \
	libion_mtk \
	libdpframework \
	libhardware \
	libgralloc_extra \
	libdl \
	libbinder \
	libpower \
	libgui

ifeq ($(MTK_M4U_SUPPORT), yes)
	LOCAL_SHARED_LIBRARIES += libm4u
endif

ifeq ($(MTK_DUAL_DISPLAY), 1)
ifneq ($(MTK_EPAPER_VENDOR),)
LOCAL_SHARED_LIBRARIES += \
	libtcon_$(MTK_EPAPER_VENDOR)
else
LOCAL_SHARED_LIBRARIES += \
	libtcon_dummy
endif
endif

ifeq ($(MTK_SEC_VIDEO_PATH_SUPPORT), yes)
LOCAL_CFLAGS += -DMTK_SVP_SUPPORT
endif # MTK_SEC_VIDEO_PATH_SUPPORT

ifneq ($(filter 1.4.0 1.4.0 1.4.0.sp 1.4.1 1.5.0 2.0.0,$(MTK_HWC_VERSION)),)
LOCAL_SHARED_LIBRARIES += \
	libged
ifneq ($(MTK_BASIC_PACKAGE), yes)
LOCAL_SHARED_LIBRARIES += \
	libui_ext
endif
endif

ifneq ($(findstring 7.,$(PLATFORM_VERSION)),)
	LOCAL_SHARED_LIBRARIES += \
		libwidevinehdcpinfo
ifneq ($(MTK_BASIC_PACKAGE), yes)
	LOCAL_SHARED_LIBRARIES += \
		libperfservicenative
endif
else
ifneq ($(MTK_BASIC_PACKAGE), yes)
	LOCAL_SHARED_LIBRARIES += \
		libhidlbase \
		libhwbinder \
		libhidltransport \
		libpq_prot \
		vendor.mediatek.hardware.pq@2.0_vendor \
		android.hardware.power@1.0 \
		vendor.mediatek.hardware.power@1.1_vendor
endif
	LOCAL_CFLAGS += -DBYPASS_WLV1_CHECKING
endif

# HAL module implemenation stored in
# hw/<OVERLAY_HARDWARE_MODULE_ID>.<ro.product.board>.so
LOCAL_MODULE := hwcomposer.$(MTK_PLATFORM_DIR)
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_OWNER := mtk

LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw
include $(MTK_SHARED_LIBRARY)

endif # MTK_HWC_SUPPORT
