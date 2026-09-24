#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

# The release version is VERSION at the repository root and nothing else, so the
# NACP, the About page and the release notes cannot drift apart. The top level
# Makefile reads it and passes it in as APP_VERSION; reading the same file here
# keeps a plain `make -C nro` working. The path is built from TOPDIR (this
# directory) rather than CURDIR because this Makefile is read twice - once here
# and once from build/ (see the bottom of the file).
#
# This sits above switch_rules on purpose: that file fills APP_VERSION in with
# "1.0.0" when it is empty, and a release that quietly went out as 1.0.0 is
# exactly what VERSION is here to prevent.
APP_VERSION_FILE	:=	$(abspath $(TOPDIR)/../VERSION)

ifeq ($(strip $(APP_VERSION)),)
APP_VERSION	:=	$(shell cat $(APP_VERSION_FILE) 2>/dev/null)
endif
ifeq ($(strip $(APP_VERSION)),)
$(error "no app version: $(APP_VERSION_FILE) is missing or empty")
endif

include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#
# NO_ICON: if set to anything, do not use icon.
# NO_NACP: if set to anything, no .nacp file is generated.
# APP_TITLE is the name of the app stored in the .nacp file (Optional)
# APP_AUTHOR is the author of the app stored in the .nacp file (Optional)
# APP_VERSION is the version of the app stored in the .nacp file (Optional)
# APP_TITLEID is the titleID of the app stored in the .nacp file (Optional)
# ICON is the filename of the icon (.jpg), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.jpg
#     - icon.jpg
#     - <libnx folder>/default_icon.jpg
#
# CONFIG_JSON is the filename of the NPDM config file (.json), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.json
#     - config.json
#   If a JSON file is provided or autodetected, an ExeFS PFS0 (.nsp) is built instead
#   of a homebrew executable (.nro). This is intended to be used for sysmodules.
#   NACP building is skipped as well.
#---------------------------------------------------------------------------------
TARGET		:=	DGLAB-NX
BUILD		:=	build
SOURCES		:=	source source/ui source/platform source/motion source/touch source/util
DATA		:=	data
INCLUDES	:=	include ../common/include ../third_party
#ROMFS	:=	romfs

# What the homebrew menu and the NACP say (AGENTS.md §15). The name and the
# author are written down here; the version is not - it is VERSION at the
# repository root, which is read below.
APP_TITLE	:=	DGLAB-NX
APP_AUTHOR	:=	livcm
# The icon the menu draws, in the UI's own palette (background #2D2D2D, accent
# #00FFC8). nro/tools/make_icon.c draws it; its header has the commands.
ICON		:=	DGLAB-NX.jpg

# Release layout (see AGENTS.md): the NRO is copied next to the sysmodule
# package under build/ at the repository root, in a directory of its own that
# mirrors what goes on the SD card: the .nro and the lang/ it reads at run time.
RELEASE_DIR	:=	$(CURDIR)/../build
RELEASE_APP_DIR	:=	$(RELEASE_DIR)/$(TARGET)
RELEASE_NRO	:=	$(RELEASE_APP_DIR)/$(TARGET).nro
LANG_DIR	:=	$(CURDIR)/../lang
RELEASE_LANG_DIR	:=	$(RELEASE_APP_DIR)/lang

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# Which build this is, so the About page answers "is the console running the
# build I just made?" - the same stamp the sysmodule logs (dglab/build.h).
BUILD_STAMP	:=	$(shell git describe --always --dirty 2>/dev/null || echo unknown)

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ '-DDGLAB_APP_VERSION="$(APP_VERSION)"' \
			'-DDGLAB_BUILD_STAMP="$(BUILD_STAMP)"'

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# -lm is not optional: third_party/stb/stb_truetype.h calls sqrt, and the
# devkitA64 the CI container ships (15.2.0) does not pull libm in by itself -
# the link failed there with "undefined reference to `sqrt'" from text_ttf.o.
# The one on macOS (16.1.0) links without it, so this only ever showed up in CI.
LIBS	:= -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
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

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

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

.PHONY: $(BUILD) clean all package

#---------------------------------------------------------------------------------
all: package

package: $(BUILD)
	@test -f $(OUTPUT).nro || { echo "error: $(OUTPUT).nro not found"; exit 1; }
	@test -n "$(wildcard $(LANG_DIR)/*.json)" || \
		{ echo "error: no language files in $(LANG_DIR)"; exit 1; }
	@mkdir -p $(RELEASE_APP_DIR)
	@cp $(OUTPUT).nro $(RELEASE_NRO)
	@mkdir -p $(RELEASE_LANG_DIR)
	@cp $(LANG_DIR)/*.json $(RELEASE_LANG_DIR)
	@echo "released ... $(RELEASE_NRO)  [$(APP_TITLE) $(APP_VERSION), $(BUILD_STAMP)]"
	@echo "released ... $(RELEASE_LANG_DIR)/*.json"

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(RELEASE_NRO) \
		$(RELEASE_APP_DIR)
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif


#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

# Two things the .nro carries that libnx does not watch: a new release version
# (the .nacp keeps whatever string it was built with) and a new icon. Bumping
# VERSION or redrawing the icon has to rebuild them.
$(OUTPUT).nacp	:	$(APP_VERSION_FILE)
$(OUTPUT).nro	:	$(APP_ICON)

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

# The release version is compiled into main.o (the About page shows it) as well
# as written into the NACP, so a new VERSION has to rebuild that object too.
# Without this the metadata would move to the new release while the page kept
# showing the old one.
main.o	:	$(APP_VERSION_FILE)

# The build stamp reaches the sources through -D, and make does not treat a
# changed command line as a reason to rebuild, so the About page could keep
# showing the previous checkout's stamp from a stale object. Writing the stamp
# (and the release version) to a file only when it changes turns that into an
# ordinary prerequisite, and the objects are backdated when it does change so the
# rebuild does not depend on the clock (two builds inside the same second would
# otherwise compare equal and keep a half-updated binary).
STAMP_FILE	:=	$(CURDIR)/build-stamp
$(OFILES_SRC)	:	$(STAMP_FILE)

.PHONY: FORCE
$(STAMP_FILE):	FORCE
	@printf '%s %s\n' '$(APP_VERSION)' '$(BUILD_STAMP)' | cmp -s - $@ || { \
		printf '%s %s\n' '$(APP_VERSION)' '$(BUILD_STAMP)' > $@; \
		touch -t 198001010000 $(OFILES_SRC); \
	}
FORCE:

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
