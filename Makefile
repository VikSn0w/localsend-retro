# On the PPC Leopard Mac (Xcode 3.1 gcc) just run:  make app
# On a modern Mac:                                    make cli   (protocol test tool)
# Add NOTLS=1 to build without HTTPS (no mbedTLS needed).

CC      ?= gcc
CFLAGS  += -std=gnu99 -O2 -Wall
LDLIBS  += -lpthread

MBED    = third_party/src/mbedtls-3.6.7
CORE    = src/ls.c src/ls_json.c src/ls_tls.c src/ls_compat.c src/plat_posix.c src/ls_https.c src/ls_update.c

ifeq ($(NOTLS),1)
TLSDEPS =
else
CFLAGS += -DLS_WITH_TLS -I$(MBED)/include
LDLIBS += -L$(MBED)/library -lmbedtls -lmbedx509 -lmbedcrypto
TLSDEPS = $(MBED)/library/libmbedtls.a
endif

$(MBED)/library/libmbedtls.a:
	$(MAKE) -C $(MBED)/library CC="$(CC)" CFLAGS="-std=gnu99 -O2" static

cli: $(CORE) src/cli.c src/ls.h src/ls_json.h src/ls_tls.h $(TLSDEPS)
	$(CC) $(CFLAGS) -o ls-cli $(CORE) src/cli.c $(LDLIBS)

app: LocalSend.app/Contents/MacOS/LocalSend

LocalSend.app/Contents/MacOS/LocalSend: $(CORE) src/AppController.m src/main.m src/ls.h src/ls_plat.h $(TLSDEPS)
	mkdir -p LocalSend.app/Contents/MacOS LocalSend.app/Contents/Resources
	cp Info.plist LocalSend.app/Contents/Info.plist
	cp assets/AppIcon.icns assets/cacert.pem LocalSend.app/Contents/Resources/
	$(CC) $(CFLAGS) -o $@ $(CORE) src/AppController.m src/main.m \
		-framework Cocoa $(LDLIBS)

clean:
	rm -rf ls-cli LocalSend.app received

.PHONY: cli app clean

# update-check test tool (needs the TLS build)
updtest: $(CORE) src/updtest.c src/ls_update.c src/ls_https.c src/update_key.h $(TLSDEPS)
	$(CC) $(CFLAGS) -o ls-updtest $(CORE) src/updtest.c $(LDLIBS)

.PHONY: updtest
