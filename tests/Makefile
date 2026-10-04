CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -std=gnu99 -fsanitize=address,undefined
CPPFLAGS += -I../PluginManager/src -I../vendor/quickjs -DFLOW_NO_ATOMICS -D_GNU_SOURCE -DCONFIG_VERSION=\"2026-06-04\"
LDLIBS ?= -lcjson -ljpeg -lpng -lgif -lm
CORE = ../src/url.c ../src/document.c ../src/dom.c ../src/css.c ../src/script.c ../src/session.c ../src/layout.c ../src/view.c ../src/picture.c
QJS = quickjs cutils dtoa libregexp libunicode
QOBJS = $(addprefix qjs_,$(addsuffix .o,$(QJS)))
.PHONY: check clean
check: test_browser test_engine test_view test_picture test_adopt test_jar transport
	./test_browser
	./test_engine
	./test_view
	./test_picture
	./test_adopt
	./test_jar
	python3 test_transport.py
../src/dom_bootstrap.h: ../src/dom_bootstrap.js ../tools/embed_js.py
	python3 ../tools/embed_js.py $< $@
qjs_%.o: ../vendor/quickjs/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-parameter -Wno-sign-compare -c $< -o $@
test_browser: test_browser.c $(CORE) $(QOBJS) ../src/dom_bootstrap.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_browser.c $(CORE) $(QOBJS) $(LDLIBS)
test_engine: test_engine.c $(CORE) $(QOBJS) ../src/dom_bootstrap.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_engine.c $(CORE) $(QOBJS) $(LDLIBS)
test_view: test_view.c $(CORE) $(QOBJS) ../src/dom_bootstrap.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_view.c $(CORE) $(QOBJS) $(LDLIBS)
test_picture: test_picture.c $(CORE) $(QOBJS) ../src/dom_bootstrap.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_picture.c $(CORE) $(QOBJS) $(LDLIBS)
test_adopt: test_adopt.c ../src/adopt.c ../PluginManager/src/fs.c ../PluginManager/src/util.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPM_FS_TESTING -o $@ $^ -lmbedcrypto
test_jar: test_jar.c ../PluginManager/src/jar.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lcurl
transport: transport.c ../PluginManager/src/http_policy.c ../PluginManager/src/transfer.c
	$(CC) $(CFLAGS) -o $@ $^ -lcurl
clean:
	rm -f test_browser test_engine test_view test_picture test_adopt test_jar transport qjs_*.o
