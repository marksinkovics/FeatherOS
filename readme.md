# Feather OS

 Hobby x86-64 bit operating system with the purpose of learning about kernel and operating system development.

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
   `./build.sh all`
   In the case of successful build the script will run the build kernel in QEMU
3. Run the built kernel in QEMU
   `./build.sh run`

## Code Quality

Install the formatting and lint tools listed in [development requirements](docs/development.md). Format the source with `bash tools/quality.sh format`, then run `bash tools/quality.sh check`. Install the pre-commit hook with `pre-commit install`; it blocks commits unless the configured checks pass.

## Screenshot

![Feather OS](docs/assets/FeatherOS.png)