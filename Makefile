#---------------------------------------------------------------------------------
# DGLAB-NX top level build.
#
#   make            build every component into release/
#   make release    same as make
#   make clean      remove build outputs
#
# Release layout (see AGENTS.md):
#
#   release/
#   ├── <TITLE_ID>/        Atmosphère sysmodule: exefs.nsp, toolbox.json, flags/boot2.flag
#   ├── DGLAB-NX.nro       homebrew front end
#   └── DGLAB-NX-Ovl.ovl   Tesla / Ultrahand overlay (not implemented yet)
#
# The sysmodule Title ID is not written down here: sysmodule/ derives the
# directory name from sysmodule/DGLAB-NX-Core.json, which stays the single source of
# truth for it.
#---------------------------------------------------------------------------------

RELEASE_DIR := $(CURDIR)/release

.PHONY: all release sysmodule nro overlay clean

all: release

release: sysmodule nro overlay
	@echo "release layout ready in $(RELEASE_DIR)"

sysmodule:
	@$(MAKE) -C sysmodule package

nro:
	@$(MAKE) -C nro package

# The overlay component has no implementation yet (see overlay/AGENTS.md). It is
# part of the release layout, so it is built automatically once overlay/Makefile
# exists.
overlay:
	@if [ -f overlay/Makefile ]; then \
		$(MAKE) -C overlay package; \
	else \
		echo "note: overlay not implemented yet, DGLAB-NX-Ovl.ovl is not produced"; \
	fi

clean:
	@$(MAKE) -C sysmodule clean
	@$(MAKE) -C nro clean
	@if [ -f overlay/Makefile ]; then $(MAKE) -C overlay clean; fi
	@rm -rf $(RELEASE_DIR)
