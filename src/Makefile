CC  := x86_64-elf-gcc
CXX := x86_64-elf-g++
LD  := x86_64-elf-ld

CFLAGS = 	-g -ffreestanding \
			-falign-jumps -falign-functions -falign-labels -falign-loops \
			-fstrength-reduce -fomit-frame-pointer -finline-functions \
			-Wno-unused-function -fno-builtin -Werror -Wno-unused-label -Wno-cpp -Wno-unused-parameter \
			-nostdlib -nostartfiles -nodefaultlibs \
			-Wall -Wextra -O0 -Iinc \
			-fno-stack-protector -fno-pic \
			-mcmodel=kernel

LDFLAGS = 	-T linker.ld \
			-nostdlib \
			-zmax-page-size=0x1000 \
			-static

override C_FILES := $(shell find ./ -type f -name '*.c')
override CXX_FILES := $(shell find ./ -type f -name '*.cpp')
override OBJ := $(C_FILES:.c=.o) $(CXX_FILES:.cpp=.o)
override KERNEL := kernel.elf

.PHONY: all
all: $(KERNEL)

$(KERNEL): $(OBJ)
	$(LD) $(OBJ) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) -I ../deps/stivale -I . $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) -I ../deps/stivale -I . $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(KERNEL) $(OBJ)
