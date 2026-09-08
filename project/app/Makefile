
MAKEFILE_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
export LC_ALL=C
SHELL:=/bin/bash
MAKE=make
AT?=@

################################################################################
##   Platform Specified  Configuraton
################################################################################
include $(MAKEFILE_DIR)/Makefile.param

# build.sh normally exports this path.  Keep the product Makefile usable on
# its own as well, and avoid silently dropping staged application files.
RK_PROJECT_PATH_APP ?= $(RK_PROJECT_OUTPUT)/app_out

################################################################################
##   Public Configuraton
################################################################################

ifeq ($(RK_FACIAL_RECOGNITION_APP),y)
app_src := ./facial_recognition/Makefile
else
app_src := $(wildcard ./*/Makefile ./component/fastboot_server/Makefile)
endif
app_src := $(dir $(app_src))

ifeq ($(RK_FACIAL_RECOGNITION_APP),y)
all: facial-recognition-build

facial-recognition-build:
	@rm -rf "$(RK_PROJECT_PATH_APP)"
	$(foreach target,$(app_src),env -u RK_APP_OUTPUT $(MAKE) -C $(target)||exit -1;)
	$(call MAROC_COPY_PKG_TO_APP_OUTPUT, $(RK_PROJECT_PATH_APP), $(RK_APP_OUTPUT))
	test -f "$(RK_APP_OUTPUT)/usr/bin/RkLunch.sh" || \
	test -f "$(RK_APP_OUTPUT)/bin/RkLunch.sh" || \
	(mkdir -p "$(RK_APP_OUTPUT)/bin" && cp -av src/RkLunch* "$(RK_APP_OUTPUT)/bin")
else
all: $(pkg-build)
	$(foreach target,$(app_src),make -C $(target)||exit -1;)
	$(call MAROC_COPY_PKG_TO_APP_OUTPUT, $(RK_PROJECT_PATH_APP), $(RK_APP_OUTPUT))
	test -f "$(RK_APP_OUTPUT)/usr/bin/RkLunch.sh" || \
	test -f "$(RK_APP_OUTPUT)/bin/RkLunch.sh" || \
	(mkdir -p "$(RK_APP_OUTPUT)/bin" && cp -av src/RkLunch* "$(RK_APP_OUTPUT)/bin")
endif


distclean: clean

clean:
	$(foreach target,$(app_src),make clean -C $(target)||exit -1;)
	@rm -rf $(RK_APP_OUTPUT)

info:
	@$(foreach target,$(app_src),make -C $(target) info||exit 0;)
