TARGET = arkbrowser
OBJS = src/main.o src/jobs.o src/url.o src/document.o src/dom.o src/css.o src/script.o src/layout.o src/session.o \
       vendor/quickjs/quickjs.o vendor/quickjs/cutils.o vendor/quickjs/dtoa.o vendor/quickjs/libregexp.o vendor/quickjs/libunicode.o \
       shared/net.o shared/http_policy.o shared/transfer.o shared/tlsdiag.o shared/clock.o shared/entropy.o \
       shared/stubs.o shared/resume.o shared/fs.o shared/util.o shared/gfx.o shared/text.o shared/input.o
INCDIR = src vendor/quickjs ../PluginManager/src
CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -std=gnu99 -DARKB_NO_ATOMICS -D_GNU_SOURCE -DCONFIG_VERSION=\"2026-06-04\" $(EXTRA_CFLAGS)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 660
# Leave sceUtility/RTC/net_inet/resolver to the SDK's libc dependencies, once.
LIBS = -lintrafont -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lcjson -lz \
       -lpspgu -lpspgum -lpsppower -lpspwlan -lpspnet -lpspnet_apctl -lm
EXTRA_TARGETS = check-imports EBOOT.PBP
PSP_EBOOT_TITLE = ARK Browser
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

shared/%.o: ../PluginManager/src/%.c
	mkdir -p shared
	$(CC) $(CFLAGS) -c $< -o $@
shared/%.o: ../PluginManager/src/%.S
	mkdir -p shared
	$(CC) $(ASFLAGS) -c $< -o $@
src/dom_bootstrap.h: src/dom_bootstrap.js tools/embed_js.py
	python3 tools/embed_js.py $< $@
src/script.o: src/dom_bootstrap.h
.PHONY: check-imports package
check-imports: $(TARGET).elf
	python3 ../PluginManager/tools/check_imports.py $(TARGET).elf
package: check-imports EBOOT.PBP
	rm -rf dist
	mkdir -p dist/PSP/GAME/ARKBrowser
	cp vendor/quickjs/LICENSE dist/PSP/GAME/ARKBrowser/QuickJS-LICENSE
	cp EBOOT.PBP README.md dist/PSP/GAME/ARKBrowser/
	cp ../XMBControl/LICENSE dist/PSP/GAME/ARKBrowser/COPYING
	cp ../PluginManager/res/cacert.pem dist/PSP/GAME/ARKBrowser/
	cd dist && zip -q -r ARKBrowser.zip PSP
