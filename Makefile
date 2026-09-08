ifeq ($(APP_PARAM),)
APP_PARAM := ../Makefile.param
include $(APP_PARAM)
endif

SHELL := /bin/bash
PKG_NAME := rv06_dashcam
WEB_NAME := rv06_webconfig
SDK_VI_VENC_PROBE := ../../../media/samples/simple_test/out/bin/simple_vi_bind_venc_wrap_rv1106
PKG_BIN := out
BUILD_DIR := build
FIRMWARE_VERSION := $(shell cat ../../../FIRMWARE_VERSION 2>/dev/null || echo unknown)
JSONCPP_SYSROOT := ../../../sysdrv/source/buildroot/buildroot-2024.02.10/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot
SRCS := src/main.cpp src/dashcam_log.cpp src/ec25_gps_manager.cpp \
  src/rtsp_stream_manager.cpp src/driver_uart_manager.cpp \
  src/jt808_protocol.cpp src/jt1078_manager.cpp src/jt808_client.cpp
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
WEB_OBJ := $(BUILD_DIR)/web_config_server.o
DRIVER_TEST := $(BUILD_DIR)/driver_uart_manager_test
JT808_TEST := $(BUILD_DIR)/jt808_protocol_test
JT_MOCK := $(BUILD_DIR)/jt_cmsv6_mock

CXXFLAGS := $(RK_APP_CROSS_CFLAGS) -std=c++17 -Wall -Wextra -Werror -MMD -MP \
  -I$(RK_PROJECT_OUTPUT)/media_out/include \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq/common \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq/xcore \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq/algos \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq/iq_parser \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq/iq_parser_v2 \
  -I$(RK_PROJECT_OUTPUT)/media_out/include/rkaiq/uAPI2 \
  -I$(JSONCPP_SYSROOT)/usr/include
LDFLAGS := -L$(RK_PROJECT_OUTPUT)/media_out/lib \
  -L$(JSONCPP_SYSROOT)/usr/lib \
  -Wl,-rpath-link,$(RK_PROJECT_OUTPUT)/media_out/lib \
  -lrockit_full -lrockit -lrockchip_mpp -lrkaiq -lrga -lrtsp -ljsoncpp -pthread

.PHONY: all clean install test
all:
	@$(MAKE) --no-print-directory install

install: $(PKG_NAME) $(WEB_NAME)
	@mkdir -p $(PKG_BIN)/bin $(PKG_BIN)/etc/dashcam $(PKG_BIN)/share/dashcam
	@cp -f $(PKG_NAME) $(PKG_BIN)/bin/
	@cp -f $(WEB_NAME) $(PKG_BIN)/bin/
	@cp -f $(SDK_VI_VENC_PROBE) $(PKG_BIN)/bin/
	@cp -f dashcam.sh $(PKG_BIN)/bin/
	@cp -f config/config.default.json $(PKG_BIN)/etc/dashcam/config.json
	@cp -f config/runtime.default.conf $(PKG_BIN)/etc/dashcam/runtime.conf
	@cp -f config/jt808.default.conf $(PKG_BIN)/etc/dashcam/jt808.conf
	@cp -f config/admin.password.hash $(PKG_BIN)/etc/dashcam/admin.password.hash
	@cp -a www $(PKG_BIN)/share/dashcam/
	@cp -f scripts/validate_dual_mipi.sh $(PKG_BIN)/bin/
	@cp -f scripts/validate_cam0_raw.sh $(PKG_BIN)/bin/
	@cp -f scripts/rv06_storage_action.sh $(PKG_BIN)/bin/
	@cp -f scripts/rv06_config_migrate_prepare.sh $(PKG_BIN)/bin/
	@chmod 0755 $(PKG_BIN)/bin/$(PKG_NAME) $(PKG_BIN)/bin/$(WEB_NAME) $(PKG_BIN)/bin/simple_vi_bind_venc_wrap_rv1106 $(PKG_BIN)/bin/dashcam.sh $(PKG_BIN)/bin/validate_dual_mipi.sh $(PKG_BIN)/bin/validate_cam0_raw.sh $(PKG_BIN)/bin/rv06_storage_action.sh $(PKG_BIN)/bin/rv06_config_migrate_prepare.sh
	@chmod 0600 $(PKG_BIN)/etc/dashcam/admin.password.hash
	@chmod 0600 $(PKG_BIN)/etc/dashcam/jt808.conf
	@chmod 0644 $(PKG_BIN)/etc/dashcam/runtime.conf
	$(call MAROC_COPY_PKG_TO_APP_OUTPUT,$(RK_APP_OUTPUT),$(PKG_BIN))

$(PKG_NAME): $(OBJS)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) $^ -o $@ $(LDFLAGS)

$(WEB_NAME): $(WEB_OBJ)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) $^ -o $@ -lcrypt

$(WEB_OBJ): src/web_config_server.cpp
	@mkdir -p $(dir $@)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) -std=c++17 -Wall -Wextra -Werror \
	  -DRV06_FIRMWARE_VERSION='"$(FIRMWARE_VERSION)"' -c $< -o $@

$(DRIVER_TEST): tests/driver_uart_manager_test.cpp \
               $(BUILD_DIR)/driver_uart_manager.o $(BUILD_DIR)/dashcam_log.o
	$(RK_APP_CROSS)-g++ $(CXXFLAGS) -Isrc $^ -o $@ \
	  -L$(JSONCPP_SYSROOT)/usr/lib -ljsoncpp -pthread

$(JT808_TEST): tests/jt808_protocol_test.cpp $(BUILD_DIR)/jt808_protocol.o
	$(RK_APP_CROSS)-g++ $(CXXFLAGS) -Isrc $^ -o $@

$(JT_MOCK): tests/jt_cmsv6_mock.cpp $(BUILD_DIR)/jt808_protocol.o
	$(RK_APP_CROSS)-g++ $(CXXFLAGS) -Isrc $^ -o $@

test: $(DRIVER_TEST) $(JT808_TEST) $(JT_MOCK)
	@echo "Built target test: $(DRIVER_TEST)"
	@echo "Built target test: $(JT808_TEST)"
	@echo "Built target mock: $(JT_MOCK)"

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(RK_APP_CROSS)-g++ $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) $(PKG_BIN) $(PKG_NAME) $(WEB_NAME)

-include $(OBJS:.o=.d)
