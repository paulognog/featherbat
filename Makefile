CC      ?= gcc
WINDRES ?= windres
CFLAGS  ?= -std=c11 -Os -nostdlib -ffreestanding -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -fno-ident -ffunction-sections -mno-stack-arg-probe \
           -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra
LDFLAGS ?= -mwindows -nostdlib -s -Wl,--gc-sections -Wl,-e,wWinMainCRTStartup
LIBS    := -lkernel32 -luser32 -lgdi32 -ladvapi32 -lsetupapi -lcfgmgr32 -lhid -lshell32 -lcomctl32

SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c,build/%.o,$(SRC)) build/app_res.o

featherbat.exe: $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(LIBS)

build/%.o: src/%.c $(wildcard src/*.h)
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -c $< -o $@

build/app_res.o: res/app.rc res/app.manifest res/app.ico
	@if not exist build mkdir build
	$(WINDRES) -i $< -o $@ --include-dir res

clean:
	@if exist build rmdir /s /q build
	@if exist featherbat.exe del /q featherbat.exe

test: $(OBJ)
	@if not exist build mkdir build
	$(CC) $(CFLAGS) -mconsole -s -Wl,--gc-sections -Wl,-e,mainCRTStartup tests/unit_test.c build/mini.o build/hid.o build/drv_logi.o build/drv_ps.o build/drv_kysona.o -o build/unit_test.exe -lkernel32 -luser32 -lsetupapi -lcfgmgr32 -lhid
	./build/unit_test.exe

.PHONY: clean test
