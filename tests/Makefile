CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -std=c99 -fsanitize=address,undefined
CORE = ../src/url.c ../src/document.c ../src/session.c
.PHONY: check clean
check: test_browser transport
	./test_browser
	python3 test_transport.py
test_browser: test_browser.c $(CORE)
	$(CC) $(CFLAGS) -o $@ $^
transport: transport.c ../../PluginManager/src/http_policy.c ../../PluginManager/src/transfer.c
	$(CC) $(CFLAGS) -o $@ $^ -lcurl
clean:
	rm -f test_browser transport
