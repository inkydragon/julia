## libssh2
ifneq ($(USE_BINARYBUILDER_LIBSSH2), 1)
LIBSSH2_GIT_URL := https://github.com/libssh2/libssh2.git
LIBSSH2_TAR_URL = https://api.github.com/repos/libssh2/libssh2/tarball/$1
$(eval $(call git-external,libssh2,LIBSSH2,CMakeLists.txt,,$(SRCCACHE)))

ifeq ($(USE_SYSTEM_MBEDTLS), 0)
$(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-configured: | $(build_prefix)/manifest/mbedtls
endif

LIBSSH2_OPTS := $(CMAKE_COMMON) -DBUILD_SHARED_LIBS=ON -DBUILD_EXAMPLES=OFF \
		-DCMAKE_BUILD_TYPE=Release

ifeq ($(BUILD_OS),WINNT)
LIBSSH2_OPTS += -DCRYPTO_BACKEND=WinCNG -DENABLE_ZLIB_COMPRESSION=OFF
LIBSSH2_OPTS += -G"MSYS Makefiles"
else
LIBSSH2_OPTS += -DCRYPTO_BACKEND=mbedTLS -DENABLE_ZLIB_COMPRESSION=OFF
endif

ifneq (,$(findstring $(OS),Linux FreeBSD))
LIBSSH2_OPTS += -DCMAKE_INSTALL_RPATH="\$$ORIGIN"
endif

ifeq ($(LIBSSH2_ENABLE_TESTS), 0)
LIBSSH2_OPTS += -DBUILD_TESTING=OFF
endif

LIBSSH2_SRC_PATH := $(SRCCACHE)/$(LIBSSH2_SRC_DIR)

 # Apply patch to fix v1.10.0 CVE (https://github.com/libssh2/libssh2/issues/649), drop with v1.11
$(LIBSSH2_SRC_PATH)/libssh2-userauth-check.patch-applied: $(LIBSSH2_SRC_PATH)/source-extracted
	cd $(LIBSSH2_SRC_PATH) && \
		patch -p1 -f < $(SRCDIR)/patches/libssh2-userauth-check.patch
	echo 1 > $@

$(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-configured: \
	$(LIBSSH2_SRC_PATH)/libssh2-userauth-check.patch-applied

$(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-configured: $(LIBSSH2_SRC_PATH)/source-extracted
	mkdir -p $(dir $@)
	cd $(dir $@) && \
	$(CMAKE) $(dir $<) $(LIBSSH2_OPTS)
	echo 1 > $@

$(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-compiled: $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-configured
	$(MAKE) -C $(dir $<) libssh2
	echo 1 > $@

$(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-checked: $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-compiled
ifeq ($(OS),$(BUILD_OS))
	$(MAKE) -C $(dir $@) test
endif
	echo 1 > $@

$(eval $(call staged-install, \
	libssh2,$(LIBSSH2_SRC_DIR), \
	MAKE_INSTALL,,, \
	$$(INSTALL_NAME_CMD)libssh2.$$(SHLIB_EXT) $$(build_shlibdir)/libssh2.$$(SHLIB_EXT)))

clean-libssh2:
	-rm -f $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-configured $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-compiled
	-$(MAKE) -C $(BUILDDIR)/$(LIBSSH2_SRC_DIR) clean


get-libssh2: $(LIBSSH2_SRC_FILE)
extract-libssh2: $(SRCCACHE)/$(LIBSSH2_SRC_DIR)/source-extracted
configure-libssh2: $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-configured
compile-libssh2: $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-compiled
fastcheck-libssh2: check-libssh2
check-libssh2: $(BUILDDIR)/$(LIBSSH2_SRC_DIR)/build-checked

else # USE_BINARYBUILDER_LIBSSH2

$(eval $(call bb-install,libssh2,LIBSSH2,false))

endif
