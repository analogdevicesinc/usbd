TARGETS := usbd usbclient

CFLAGS := -std=gnu11 -Wall
CPPFLAGS := -D_GNU_SOURCE=1
LDLIBS_USBD := -lusbg
LDLIBS_USBCLIENT := -lusb-1.0 -lpthread

PREFIX ?= /usr/local

.PHONY: all
all: $(TARGETS)

usbd: usbd.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) $(LDLIBS_USBD) -o $@

usbclient: usbclient.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) $(LDLIBS_USBCLIENT) -o $@

.PHONY: clean
clean:
	rm -f usbd.o usbclient.o $(TARGETS)

.PHONY: install
install: $(TARGETS)
	cp $(TARGETS) $(DESTDIR)$(PREFIX)/bin/
