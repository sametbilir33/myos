# MyOS

**A tiny 64-bit operating system with a graphical desktop, built from scratch.**

> ⚠️ **Experimental project.** MyOS is a work in progress and is not intended for daily use.

MyOS is a hobby operating system project focused on learning how operating systems work, from booting a kernel to interacting with hardware and drawing a graphical desktop.

The project is an experiment in low-level programming, graphics, USB hardware support, and solving problems one bug at a time.

## ✨ Features

### Kernel and boot
- 64-bit x86-64 kernel
- Limine boot protocol
- Linear framebuffer graphics
- BIOS boot support, with UEFI support depending on the ISO configuration

### Graphical desktop
- Custom bitmap mouse cursor
- Desktop background and status bar
- Dropdown application menu
- Multiple application windows
- Window focusing and closing
- Window dragging with an outline preview
- Basic real-time clock display
- GUI event queue for mouse interaction

### USB input
- xHCI USB host-controller initialization
- USB device enumeration for a connected device
- Experimental USB HID Boot Mouse support
- Interrupt-IN mouse transfers handled by the desktop loop

The current input path uses a **USB mouse**. PS/2 mouse support has been removed.

## 🧪 Current Status

MyOS can boot into a basic graphical desktop and receive mouse input through its experimental xHCI/HID implementation.

This is still an early-stage operating system. USB support is limited, and broader device compatibility, keyboard input, functional applications, and storage are not implemented yet.

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
- AI-assisted development

## 🚀 Building

The current development environment uses **MSYS2 UCRT64** on Windows.

Make sure Clang, LLD, CMake, Ninja, NASM, QEMU, xorriso, and the required Limine files are installed and available in your environment.

From the repository root, configure and build:

```bash
rm -rf build/
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake
cmake --build build
```

If the build completes successfully and produces `build/myos.iso`, launch it in QEMU with an xHCI controller and a virtual USB mouse:

```bash
qemu-system-x86_64 -machine q35 -m 256M -no-reboot -no-shutdown -serial stdio -device qemu-xhci,id=xhci -device usb-mouse,bus=xhci.0 -cdrom build/myos.iso
```

The virtual xHCI controller and USB mouse are important: launching QEMU without them does not provide the USB mouse hardware expected by the current input implementation.

Build and runtime instructions may change as the project evolves.

## 🗺️ Roadmap

- [x] Boot a 64-bit kernel with Limine
- [x] Initialize framebuffer graphics
- [x] Draw a graphical desktop and status bar
- [x] Add a custom mouse cursor
- [x] Add basic window management
- [x] Replace PS/2 mouse input with experimental USB HID Boot Mouse input
- [ ] Improve partial redraw and eliminate visual artifacts
- [ ] Add USB HID keyboard input
- [ ] Add keyboard focus and text input to GUI windows
- [ ] Implement functional desktop applications
- [ ] Build a basic terminal/shell
- [ ] Add a filesystem
- [ ] Improve memory management and broaden hardware support

## 🤖 Development Philosophy

### Highly vibe-coded.

This project is **highly vibe-coded with AI assistance**.

AI tools help with implementation, debugging, architecture experiments, and turning ideas into code. The goal is still to learn, experiment, break things, fix them, and gradually build an operating system from the ground up.

This is a hobby project, not production-ready software. Contributions, suggestions, bug reports, and constructive feedback are welcome.

## 📜 License

MyOS is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

See [`LICENSE`](LICENSE) for the complete license text.

## ❤️ Disclaimer

This project is provided for educational and experimental purposes and comes with no warranty. Run it in an emulator unless you understand the risks of testing experimental operating-system code on physical hardware.
