PKG_CONFIG?=pkg-config

PKGS="wlroots-0.20" wayland-server xkbcommon
CFLAGS_PKG_CONFIG!=$(PKG_CONFIG) --cflags $(PKGS)
CFLAGS+=$(CFLAGS_PKG_CONFIG)
LIBS!=$(PKG_CONFIG) --libs $(PKGS)

all: tilewl

tilewl.o: tilewl.c
	$(CC) -c $< -g -Werror $(CFLAGS) -I. -DWLR_USE_UNSTABLE -o $@
tilewl: tilewl.o
	$(CC) $^ -g -Werror $(CFLAGS) $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -f tilewl tilewl.o

.PHONY: all clean
