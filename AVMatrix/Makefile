# AV Matrix
#   make test    pruebas del núcleo (Linux o macOS, sin CoreAudio)
#   make cross   compilación cruzada para macOS desde Linux (requiere SDK=<ruta MacOSX.sdk> y lld)
#   En el Mac use scripts/build.sh

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
BUILD   := build
SDK     ?=
MINOS   := 12.0
XFLAGS  := -isysroot $(SDK) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Wno-four-char-constants -fuse-ld=lld

.PHONY: test test-sanitize cross dist clean

test:
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/test_core tests/test_core.c Driver/src/avm_core.c -lm -lpthread
	./$(BUILD)/test_core
	$(CC) $(CFLAGS) -o $(BUILD)/test_rate tests/test_rate.c Driver/src/avm_rate.c
	./$(BUILD)/test_rate
	$(CC) $(CFLAGS) -o $(BUILD)/test_meters tests/test_meters.c Driver/src/avm_core.c -lm -lpthread
	./$(BUILD)/test_meters
	bash tests/test_install.sh
	bash tests/test_build_script.sh
	bash tests/test_app_strings.sh

test-sanitize:
	mkdir -p $(BUILD)
	gcc $(CFLAGS) -fsanitize=address,undefined -o $(BUILD)/test_core_asan tests/test_core.c Driver/src/avm_core.c -lm -lpthread
	./$(BUILD)/test_core_asan
	gcc -std=c11 -O1 -g -fsanitize=thread -o $(BUILD)/test_core_tsan tests/test_core.c Driver/src/avm_core.c -lm -lpthread
	./$(BUILD)/test_core_tsan
	gcc $(CFLAGS) -fsanitize=address,undefined -o $(BUILD)/test_rate_asan tests/test_rate.c Driver/src/avm_rate.c
	./$(BUILD)/test_rate_asan
	gcc $(CFLAGS) -fsanitize=address,undefined -o $(BUILD)/test_meters_asan tests/test_meters.c Driver/src/avm_core.c -lm -lpthread
	./$(BUILD)/test_meters_asan
	gcc -std=c11 -O1 -g -fsanitize=thread -o $(BUILD)/test_meters_tsan tests/test_meters.c Driver/src/avm_core.c -lm -lpthread
	./$(BUILD)/test_meters_tsan

cross:
	@test -n "$(SDK)" || (echo "Indique SDK=/ruta/MacOSX.sdk" && false)
	mkdir -p $(BUILD)/cross
	for a in arm64 x86_64; do \
	  clang -target $$a-apple-macos$(MINOS) $(XFLAGS) -fvisibility=hidden -bundle -o $(BUILD)/cross/AVMatrix_$$a \
	    Driver/src/AVMatrixDriver.c Driver/src/avm_core.c Driver/src/avm_rate.c -framework CoreFoundation -framework CoreAudio || exit 1; \
	  clang -target $$a-apple-macos$(MINOS) $(XFLAGS) -o $(BUILD)/cross/avmctl_$$a \
	    Client/avmctl.c Client/avm_client.c Driver/src/avm_core.c -framework CoreAudio -framework CoreFoundation || exit 1; \
	  clang -target $$a-apple-macos$(MINOS) -isysroot $(SDK) -fobjc-arc -O2 -Wall -Wextra -Werror -Wno-four-char-constants -fuse-ld=lld \
	    -o $(BUILD)/cross/app_$$a App/AVMatrixApp.m Client/avm_client.c Driver/src/avm_core.c \
	    -framework Cocoa -framework CoreAudio -framework CoreFoundation || exit 1; \
	done

dist: cross
	rm -rf dist
	mkdir -p dist/AVMatrix.driver/Contents/MacOS "dist/AV Matrix.app/Contents/MacOS"
	cp Driver/Info.plist dist/AVMatrix.driver/Contents/Info.plist
	cp App/Info.plist "dist/AV Matrix.app/Contents/Info.plist"
	llvm-lipo-18 -create $(BUILD)/cross/AVMatrix_arm64 $(BUILD)/cross/AVMatrix_x86_64 -output dist/AVMatrix.driver/Contents/MacOS/AVMatrix
	llvm-lipo-18 -create $(BUILD)/cross/avmctl_arm64 $(BUILD)/cross/avmctl_x86_64 -output dist/avmctl
	llvm-lipo-18 -create $(BUILD)/cross/app_arm64 $(BUILD)/cross/app_x86_64 -output "dist/AV Matrix.app/Contents/MacOS/AVMatrix"
	chmod +x dist/avmctl "dist/AV Matrix.app/Contents/MacOS/AVMatrix"

clean:
	rm -rf $(BUILD)
