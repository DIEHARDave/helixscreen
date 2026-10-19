# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lazy-loading trampolines for the display and GPU libraries
#
# Included always; active when DLOPEN_GPU_LIBS=yes (mk/cross.mk, the Pi and x86
# DRM builds). Every function libdrm, libgbm, libEGL and libinput export becomes
# a symbol in our binary that dlopen()s its library on the first call, so the
# binary carries no NEEDED entry for them and starts on a system without them.
# libGLESv2 is not here: GLES2 entry points are resolved through
# include/gles2_loader.h instead.
#
# The trampolines are generated at build time by the vendored Implib.so
# (tools/implib, pinned in its README) from the target's own libraries in the
# toolchain sysroot, so they always match what the binary is compiled against.
# A call into a library that is not installed aborts the process, which is why
# every caller asks include/gpu_libs.h first.

IMPLIB_OBJS :=

ifeq ($(DLOPEN_GPU_LIBS),yes)

IMPLIB_GEN := tools/implib/implib-gen.py
IMPLIB_SYSROOT_LIBS := /usr/lib/$(TARGET_TRIPLE)
IMPLIB_SONAMES := libdrm.so.2 libgbm.so.1 libEGL.so.1 libinput.so.10
IMPLIB_DIR := $(BUILD_DIR)/implib
IMPLIB_OBJS := $(foreach lib,$(IMPLIB_SONAMES),$(IMPLIB_DIR)/$(lib).tramp.o $(IMPLIB_DIR)/$(lib).init.o)

# One run writes both files, so they share the rule (a multi-target pattern
# rule is grouped).
$(IMPLIB_DIR)/%.tramp.S $(IMPLIB_DIR)/%.init.c: $(IMPLIB_SYSROOT_LIBS)/% $(IMPLIB_GEN) tools/implib/arch/common/init.c.tpl
	$(Q)mkdir -p $(IMPLIB_DIR)
	$(ECHO) "$(CYAN)[IMPLIB]$(RESET) $*"
	$(Q)python3 $(IMPLIB_GEN) -q --target $(TARGET_TRIPLE) --outdir $(IMPLIB_DIR) $<

$(IMPLIB_DIR)/%.tramp.o: $(IMPLIB_DIR)/%.tramp.S
	$(Q)$(CC) $(TARGET_CFLAGS) -c $< -o $@

$(IMPLIB_DIR)/%.init.o: $(IMPLIB_DIR)/%.init.c
	$(Q)$(CC) $(TARGET_CFLAGS) -w -c $< -o $@

# Generated sources are kept, not deleted as intermediates, so a rebuild does
# not regenerate them.
.PRECIOUS: $(IMPLIB_DIR)/%.tramp.S $(IMPLIB_DIR)/%.init.c

# The main link (mk/rules.mk) links $^, so these join it as prerequisites.
$(TARGET): $(IMPLIB_OBJS)

endif
