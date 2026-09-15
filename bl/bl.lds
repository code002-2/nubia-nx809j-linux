/* Linker script for the NX809J EFI bootloader.
 *
 * The image is linked at address 0 and is fully position independent: the
 * entry point must be the very first byte of the flat binary that objcopy
 * produces, because the PE wrapper points the PE entry point at it and GBL
 * loads the image wherever it likes.
 */
ENTRY(efi_main)

SECTIONS
{
	. = 0;

	.text : ALIGN(8) {
		*(.text.start)
		*(.text .text.*)
	}

	.rodata : ALIGN(8) {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
	}

	.data : ALIGN(8) {
		*(.data .data.*)
		*(.sdata .sdata.*)
	}

	.bss : ALIGN(8) {
		*(.bss .bss.*)
		*(COMMON)
	}

	.rela.dyn : ALIGN(8) {
		*(.rela.dyn)
		*(.rela*)
	}

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
		*(.ARM.attributes)
		*(.debug*)
	}
}
