# Portable MinGW build — gcc/windres on PATH.
#   build.bat / build.sh  -> build/BackgroundHotkeyThing.exe
#   mingw32-make
#   mingw32-make clean

CC      = gcc
WINDRES = windres
CFLAGS  = -O2 -mwindows
LDFLAGS = -static-libgcc -mwindows -s -lshell32 -lole32 -luuid

BUILD   = build
TARGET  = $(BUILD)/BackgroundHotkeyThing.exe
OBJS    = $(BUILD)/main.o $(BUILD)/resource.o

.PHONY: all clean dirs

all: dirs $(TARGET)

dirs:
	@if not exist $(BUILD) mkdir $(BUILD)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

$(BUILD)/main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o $(BUILD)/main.o

$(BUILD)/resource.o: resource.rc BackgroundHotkeyThing.ico
	$(WINDRES) -i resource.rc -o $(BUILD)/resource.o -O coff

clean:
	-cmd /C "if exist $(BUILD) rmdir /S /Q $(BUILD)"
