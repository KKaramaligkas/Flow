CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -std=gnu99 -fsanitize=address,undefined
CPPFLAGS += -I../../PluginManager/src -I../vendor/quickjs -DARKB_NO_ATOMICS -D_GNU_SOURCE -DCONFIG_VERSION=\"2026-06-04\"
LDLIBS ?= -lcjson -lm
CORE = ../src/url.c ../src/document.c ../src/dom.c ../src/css.c ../src/script.c ../src/session.c ../src/layout.c ../src/view.c
QJS = quickjs cutils dtoa libregexp libunicode
QOBJS = $(addprefix qjs_,$(addsuffix .o,$(QJS)))
.PHONY: check clean
check: test_browser test_engine test_view transport
	./test_browser
	./test_engine
	./test_view
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
transport: transport.c ../../PluginManager/src/http_policy.c ../../PluginManager/src/transfer.c
	$(CC) $(CFLAGS) -o $@ $^ -lcurl
clean:
	rm -f test_browser test_engine test_view transport qjs_*.o
