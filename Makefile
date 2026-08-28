ifeq ($(APP_PARAM),)
APP_PARAM := ../Makefile.param
-include $(APP_PARAM)
endif

RK_APP_CROSS ?= arm-rockchip830-linux-uclibcgnueabihf
RK_APP_CROSS_CFLAGS ?= -mfpu=neon -mfloat-abi=hard

SHELL := /bin/bash
PKG_NAME := facial_recognition
PKG_BIN := out
MEDIA_OUT ?= ../../../output/out/media_out
RKNN_PATH := ../capture_ai/3rdparty/rknpu2
MODEL_SRC := ../capture_ai/model/yolov5n-face-rv1106.rknn

MEDIA_INCS := -I$(MEDIA_OUT)/include \
              -I$(MEDIA_OUT)/include/rkaiq \
              -I$(MEDIA_OUT)/include/rkaiq/uAPI2 \
              -I$(MEDIA_OUT)/include/rkaiq/common \
              -I$(MEDIA_OUT)/include/rkaiq/xcore \
              -I$(MEDIA_OUT)/include/rkaiq/algos \
              -I$(MEDIA_OUT)/include/rkaiq/iq_parser \
              -I$(MEDIA_OUT)/include/rkaiq/iq_parser_v2 \
              -I$(MEDIA_OUT)/include/rkaiq/smartIr \
              -I$(MEDIA_OUT)/include/rga \
              -I$(RKNN_PATH)/include \
              -Isrc

MEDIA_DEFS := -DRKAIQ -DUAPI2 -DRV1106_RV1103

MEDIA_LIBS := -L$(MEDIA_OUT)/lib \
              -L$(RKNN_PATH)/lib \
              -Wl,-rpath,/oem/usr/lib \
              -Wl,-rpath-link,$(MEDIA_OUT)/lib \
              -Wl,-rpath-link,$(RKNN_PATH)/lib \
              -lrockit -lrockchip_mpp -lrkaiq -lrtsp -lrga -lrknnmrt -lcrypt -lpthread -lm

WEB_BIN := fr-webconfig
WEB_SRCS := src/fr_webconfig.cpp src/fr_face_db.cpp src/fr_face_recognizer.cpp src/fr_face_detector.cpp
MEDIA_BIN := fr-media-service
MEDIA_SRCS := src/fr_media_service.cpp src/fr_face_detector.cpp src/fr_face_recognizer.cpp src/fr_face_db.cpp
RKNN_INSPECT_BIN := fr-rknn-inspect
RKNN_INSPECT_SRC := src/fr_rknn_inspect.cpp
RKNN_BENCHMARK_BIN := fr-rknn-benchmark
RKNN_BENCHMARK_SRC := src/fr_rknn_benchmark.cpp

.PHONY: all clean info

all: $(WEB_BIN) $(MEDIA_BIN) $(RKNN_INSPECT_BIN) $(RKNN_BENCHMARK_BIN)
	mkdir -p $(PKG_BIN)/usr/bin $(PKG_BIN)/usr/share/facial-recognition/model $(PKG_BIN)/usr/etc/facial-recognition $(PKG_BIN)/root/etc/init.d
	cp -f scripts/fr-camera-validate.sh $(PKG_BIN)/usr/bin/fr-camera-validate
	cp -f scripts/fr-rtsp-service.sh $(PKG_BIN)/usr/bin/fr-rtsp-service
	cp -f scripts/fr-ai-service.sh $(PKG_BIN)/usr/bin/fr-ai-service.sh
	cp -f scripts/S95facial-recognition $(PKG_BIN)/root/etc/init.d/S95facial-recognition
	cp -f scripts/S96facial-webconfig $(PKG_BIN)/root/etc/init.d/S96facial-webconfig
	cp -f scripts/S97facial-ai $(PKG_BIN)/root/etc/init.d/S97facial-ai
	cp -f $(WEB_BIN) $(PKG_BIN)/usr/bin/$(WEB_BIN)
	cp -f $(MEDIA_BIN) $(PKG_BIN)/usr/bin/$(MEDIA_BIN)
	cp -f $(MEDIA_BIN) $(PKG_BIN)/usr/bin/fr-ai-service
	cp -f $(RKNN_INSPECT_BIN) $(PKG_BIN)/usr/bin/$(RKNN_INSPECT_BIN)
	cp -f $(RKNN_BENCHMARK_BIN) $(PKG_BIN)/usr/bin/$(RKNN_BENCHMARK_BIN)
	cp -f config/rtsp.conf $(PKG_BIN)/usr/etc/facial-recognition/rtsp.conf
	cp -f config/database.json $(PKG_BIN)/usr/etc/facial-recognition/database.json
	if [ -f $(MODEL_SRC) ]; then cp -f $(MODEL_SRC) $(PKG_BIN)/usr/share/facial-recognition/model/yolov5n-face-rv1106.rknn; fi
	cp -f README.md $(PKG_BIN)/usr/share/facial-recognition/README.md
	cp -a docs $(PKG_BIN)/usr/share/facial-recognition/
	chmod 0755 $(PKG_BIN)/usr/bin/* $(PKG_BIN)/root/etc/init.d/*
	if [ -n "$(RK_APP_OUTPUT)" ] && [ "$(RK_APP_OUTPUT)" != "$(PKG_BIN)" ] && [ "$(RK_APP_OUTPUT)" != "." ]; then \
		mkdir -p $(RK_APP_OUTPUT); \
		cp -raf $(PKG_BIN)/. $(RK_APP_OUTPUT)/; \
	fi

$(WEB_BIN): $(WEB_SRCS)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) -std=c++17 -Wall -Wextra -Werror -Wno-psabi -I$(RKNN_PATH)/include -Isrc $^ -o $@ -L$(RKNN_PATH)/lib -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(RKNN_PATH)/lib -lrknnmrt -lcrypt -lpthread

$(MEDIA_BIN): $(MEDIA_SRCS)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) -std=c++17 -Wall -Wextra -Werror -Wno-psabi $(MEDIA_INCS) $(MEDIA_DEFS) $^ -o $@ $(MEDIA_LIBS)

$(RKNN_INSPECT_BIN): $(RKNN_INSPECT_SRC)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) -std=c++17 -Wall -Wextra -Werror -Wno-psabi -I$(RKNN_PATH)/include $< -o $@ -L$(RKNN_PATH)/lib -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(RKNN_PATH)/lib -lrknnmrt

$(RKNN_BENCHMARK_BIN): $(RKNN_BENCHMARK_SRC)
	$(RK_APP_CROSS)-g++ $(RK_APP_CROSS_CFLAGS) -std=c++17 -Wall -Wextra -Werror -Wno-psabi -I$(RKNN_PATH)/include $< -o $@ -L$(RKNN_PATH)/lib -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(RKNN_PATH)/lib -lrknnmrt

clean:
	rm -rf $(PKG_BIN) $(WEB_BIN) $(MEDIA_BIN) $(RKNN_INSPECT_BIN) $(RKNN_BENCHMARK_BIN)

info:
	@echo "Facial Recognition: SC3336 runtime AI recognition package"
