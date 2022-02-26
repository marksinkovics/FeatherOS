.PHONY: all
all: barebones.iso

.PHONY: run
run: barebones.iso
	qemu-system-x86_64 -m 2G -cdrom barebones.iso

stivale:
	@[ -d deps/stivale ] || git clone -b master https://github.com/stivale/stivale.git deps/stivale

limine:
	@[ -d deps/limine ] || (git clone -b v2.0-branch-binary https://github.com/limine-bootloader/limine.git deps/limine && $(MAKE) -C deps/limine)

deps: limine stivale

.PHONY: kernel
kernel: stivale
	$(MAKE) -C src

barebones.iso: limine kernel
	rm -rf iso_root
	mkdir -p iso_root
	cp 	src/kernel.elf \
		limine.cfg deps/limine/limine.sys deps/limine/limine-cd.bin deps/limine/limine-eltorito-efi.bin iso_root/
	xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-eltorito-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o barebones.iso
	deps/limine/limine-install barebones.iso
	rm -rf iso_root

.PHONY: clean
clean:
	rm -rf iso_root barebones.iso
	$(MAKE) -C src clean

fullclean: clean
	rm -rf deps/stivale
	rm -rf deps/limine
