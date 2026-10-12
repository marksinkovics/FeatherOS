# Feather OS

 Hobby operating system targeting x86-64 and AArch64, built to learn kernel and operating system development.

## Tech stack

* Compiler g++
* Language: ASM, C, C++
* Build system: CMAKE
* Bootloader: [Limine](https://github.com/limine-bootloader/limine)
* Emulator: QEMU

## Development

See the [FeatherOS roadmap](docs/roadmap.md) for planned kernel milestones.

1. Clone the project
   `git clone https://github.com/marksinkovics/FeatherOS.git`
2. Build the project with all components
   `./build.sh --arch x86_64 all`
   In the case of successful build the script will run the build kernel in QEMU
3. Run the built kernel in QEMU
   `./build.sh --arch x86_64 run`

Run the unittest-based QEMU boot and panic integration matrix for both x86-64 and AArch64 with `./build.sh test-all`.

## Code Quality

Install the formatting and lint tools listed in [development requirements](docs/development.md). Run `bash tools/quality.sh --fix` to apply available fixes. Install the pre-commit hook with `pre-commit install`; it quietly blocks commits when checks fail and shows diagnostics for issues that need manual repair.

## Screenshot

![Feather OS](docs/assets/FeatherOS.png)