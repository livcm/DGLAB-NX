#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# The module name is DGLAB-NX-Core (see AGENTS.md). TARGET also selects the
# NPDM config: libnx's template auto-detects <TARGET>.json (or config.json), so
# sysmodule/DGLAB-NX-Core.json has to keep matching it. A mismatch is not
# harmless: the build silently produces a homebrew .nro instead of a sysmodule.
TARGET		:=	DGLAB-NX-Core
BUILD		:=	build
SOURCES		:=	source source/protocol source/transport source/net
DATA		:=	data
INCLUDES	:=	include ../common/include

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lnx

LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

#---------------------------------------------------------------------------------
# Packaging (default target): build the sysmodule and produce an Atmosphère
# install directory. The title id is read from APP_JSON so it stays the single
# source of truth (it is not duplicated in this Makefile or toolbox.json).
#---------------------------------------------------------------------------------
TITLE_ID_HEX := $(shell sed -nE 's/^[[:space:]]*"title_id"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' $(APP_JSON) | head -n1)
TITLE_ID_DIR := $(shell printf '%s' '$(TITLE_ID_HEX)' | sed 's/^0x//' | tr '[:lower:]' '[:upper:]')
TITLE_ID_TID := 0x$(TITLE_ID_DIR)

# Release layout (see AGENTS.md): every build artifact lands under release/ at
# the repository root, so the SD card contents can be copied in one go.
RELEASE_DIR  := $(CURDIR)/../release
PKG_DIR      := $(RELEASE_DIR)/$(TITLE_ID_DIR)
TOOLBOX_JSON := $(PKG_DIR)/toolbox.json
FLAGS_DIR    := $(PKG_DIR)/flags
BOOT2_FLAG   := $(FLAGS_DIR)/boot2.flag

.PHONY: package all $(BUILD) clean

package: $(BUILD)
	@test -f $(OUTPUT).nsp || { echo "error: $(OUTPUT).nsp not found"; exit 1; }
	@test -n "$(TITLE_ID_HEX)" || { echo "error: title_id not found in $(APP_JSON)"; exit 1; }
	@printf '%s\n' '$(TITLE_ID_DIR)' | grep -Eq '^[0-9A-F]{16}$$' || { echo "error: invalid title id '$(TITLE_ID_DIR)'"; exit 1; }
	@rm -rf $(PKG_DIR)
	@mkdir -p $(FLAGS_DIR)
	@cp $(OUTPUT).nsp $(PKG_DIR)/exefs.nsp
	@printf '{\n  "name": "%s",\n  "tid": "%s",\n  "requires_reboot": true\n}\n' '$(TARGET)' '$(TITLE_ID_TID)' > $(TOOLBOX_JSON)
	@: > $(BOOT2_FLAG)
	@test -f $(TOOLBOX_JSON) || { echo "error: toolbox.json missing"; exit 1; }
	@test -f $(BOOT2_FLAG) || { echo "error: boot2.flag missing"; exit 1; }
	@grep -q '"name"[[:space:]]*:[[:space:]]*"$(TARGET)"' $(TOOLBOX_JSON) || { echo "error: toolbox.json name mismatch"; exit 1; }
	@TID=$$(sed -nE 's/.*"tid"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' $(TOOLBOX_JSON) | head -n1 | sed 's/^0x//' | tr '[:lower:]' '[:upper:]'); test "$$TID" = '$(TITLE_ID_DIR)' || { echo "error: toolbox.json tid mismatch"; exit 1; }
	@echo "built ... $(PKG_DIR)"

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nso $(TARGET).nsp $(TARGET).npdm \
		$(TARGET).nro $(TARGET).nacp $(PKG_DIR)

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

else

all	:	$(OUTPUT).nsp

$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
