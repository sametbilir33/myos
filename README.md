
# MyOS

**A tiny 64-bit operating system with a GUI, built from scratch.**

> ⚠️ Experimental project. Very much a work in progress.

MyOS is a hobby operating system project focused on learning how operating systems work, from booting the kernel to drawing a graphical desktop.

It's an experiment in low-level programming, graphics, hardware interaction, and figuring things out one bug at a time.

## ✨ Features

- 64-bit x86 kernel
- Limine boot protocol
- Linear framebuffer graphics
- Custom bitmap mouse cursor
- Basic graphical desktop and status bar
- Dropdown application menu
- Multiple application windows
- Window focusing and closing
- Classic window dragging with an outline preview
- Basic real-time clock display
- Experimental PS/2 mouse input
- BIOS boot support, with UEFI boot support depending on the ISO configuration

## 🧪 Current Status

MyOS is nowhere near a daily-driver operating system. It's an evolving experiment.

The desktop and window management are under development.

Expect bugs, missing features, rough edges, and the occasional "why did that happen?" moment.

## 🛠️ Built With

- C
- x86-64 architecture
- Limine bootloader
- Clang / LLVM
- CMake
- Ninja
- NASM
- QEMU
- A questionable amount of AI-generated code

## 🚀 Building

The current development environment uses **MSYS2 UCRT64** on Windows.

Make sure Clang, LLD, CMake, Ninja, NASM, QEMU, xorriso, and the required Limine files are installed and available in your environment.

From the repository root, configure and build:

```bash
rm -rf build/
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake
cmake --build build
```

If the build completes successfully and produces `build/myos.iso`, launch it with QEMU:

```bash
qemu-system-x86_64 -machine pc -m 256M -no-reboot -no-shutdown -serial stdio -cdrom build/myos.iso
```

Build instructions may change as the project evolves.

## 🗺️ Roadmap

- [x] Boot a 64-bit kernel
- [x] Initialize framebuffer graphics
- [x] Draw a graphical desktop
- [x] Add mouse cursor support
- [x] Add basic window management
- [ ] Improve partial redraw and eliminate visual artifacts
- [ ] Add keyboard input
- [ ] Implement functional applications
- [ ] Add a filesystem
- [ ] Build a basic shell
- [ ] Improve memory management and hardware support

## 🤖 Development Philosophy

### Highly vibe-coded.

This project is **highly vibe-coded with AI assistance**.

AI tools help with implementation, debugging, architecture experiments, and turning ideas into code. The goal is still to learn, experiment, break things, fix them, and gradually build an operating system from the ground up.

This is a hobby project, not production-ready software. Contributions, suggestions, bug reports, and constructive feedback are welcome.

## 📜 License

MyOS is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

See [`LICENSE`](LICENSE) for the complete license text.

## ❤️ Disclaimer

This project is provided for educational and experimental purposes. It comes with no warranty. Run it in an emulator unless you understand the risks of testing experimental operating system code on physical hardware.
