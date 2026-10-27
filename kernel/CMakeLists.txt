find_program(XORRISO_EXECUTABLE xorriso REQUIRED)

set(LIMINE_DIR "${CMAKE_SOURCE_DIR}/boot/limine")
set(LIMINE_CONFIG "${CMAKE_SOURCE_DIR}/boot/limine.conf")
set(LIMINE_INSTALLER "${LIMINE_DIR}/limine")

if(NOT EXISTS "${LIMINE_INSTALLER}")
    message(FATAL_ERROR
        "Limine installer not found: ${LIMINE_INSTALLER}\n"
        "Run: make -C boot/limine"
    )
endif()

# Automatically discover all kernel C source files.
file(GLOB_RECURSE KERNEL_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)

add_library(kernel_core OBJECT ${KERNEL_SOURCES})

target_include_directories(kernel_core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_options(kernel_core PRIVATE
    -ffreestanding
    -fno-stack-protector
    -mno-red-zone
    -fno-pic
    -fno-pie
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -mcmodel=kernel
    -mgeneral-regs-only
)

set(KERNEL_ELF "${CMAKE_BINARY_DIR}/kernel.elf")

add_custom_command(
    OUTPUT "${KERNEL_ELF}"
    COMMAND ld.lld
        -flavor gnu
        -m elf_x86_64
        -T "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
        --build-id=none
        -o "${KERNEL_ELF}"
        $<TARGET_OBJECTS:kernel_core>
    DEPENDS
        kernel_core
        "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_custom_target(kernel DEPENDS "${KERNEL_ELF}")

set(ISO_ROOT "${CMAKE_BINARY_DIR}/iso_root")
set(ISO_FILE "${CMAKE_BINARY_DIR}/myos.iso")

add_custom_target(iso ALL
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${ISO_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${ISO_ROOT}"

    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${ISO_ROOT}/boot/limine"
        "${ISO_ROOT}/EFI/BOOT"

    COMMAND "${CMAKE_COMMAND}" -E copy
        "${KERNEL_ELF}"
        "${ISO_ROOT}/boot/kernel.elf"

    COMMAND "${CMAKE_COMMAND}" -E copy
        "${LIMINE_CONFIG}"
        "${ISO_ROOT}/boot/limine/limine.conf"

    COMMAND "${CMAKE_COMMAND}" -E copy
        "${LIMINE_DIR}/limine-bios.sys"
        "${LIMINE_DIR}/limine-bios-cd.bin"
        "${LIMINE_DIR}/limine-uefi-cd.bin"
        "${ISO_ROOT}/boot/limine"

    COMMAND "${CMAKE_COMMAND}" -E copy
        "${LIMINE_DIR}/BOOTX64.EFI"
        "${ISO_ROOT}/EFI/BOOT/BOOTX64.EFI"

    COMMAND "${XORRISO_EXECUTABLE}"
        -as mkisofs
        -R -r -J
        -b boot/limine/limine-bios-cd.bin
        -no-emul-boot
        -boot-load-size 4
        -boot-info-table
        -hfsplus
        -apm-block-size 2048
        --efi-boot boot/limine/limine-uefi-cd.bin
        -efi-boot-part
        --efi-boot-image
        --protective-msdos-label
        "${ISO_ROOT}"
        -o "${ISO_FILE}"

    COMMAND "${LIMINE_INSTALLER}"
        bios-install "${ISO_FILE}"

    DEPENDS
        kernel
        "${LIMINE_CONFIG}"
        "${LIMINE_DIR}/BOOTX64.EFI"
        "${LIMINE_DIR}/limine-bios.sys"
        "${LIMINE_DIR}/limine-bios-cd.bin"
        "${LIMINE_DIR}/limine-uefi-cd.bin"

    VERBATIM
)