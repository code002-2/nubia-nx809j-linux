// SPDX-License-Identifier: GPL-2.0
//
// NX809J EFI payload runner: boots a mainline arm64 kernel with no EFI
// services at all.
//
// Background. The stock ABL verifies whatever it boots against the OEM vbmeta
// (flipping one padding byte in boot_a is enough to have the slot marked
// unbootable), so the kernel cannot live there. The one unverified executable
// slot is `efisp`: XBL starts the UEFI application in it, and that application
// ("GBL") reads a boot menu from the ESP inside `persist`, so dropping a file
// into that ESP adds a menu entry.
//
// Two attempts at a "normal" UEFI application failed on this board: the first
// was refused by GBL's loader ("boot failed unsupported" - no relocation
// directory), and the version with a proper .reloc loaded but then took the
// device to the vendor memory dump before it managed to write its first
// progress marker, i.e. it died on its first EFI service call. GBL's own log
// shows it read the menu entry and enumerated file systems, then nothing.
//
// Hence this shape: no SystemTable, no BootServices, no console, no
// ExitBootServices, no block or file protocols. The kernel (gzipped), the
// devicetree and the initramfs are embedded in this image; the kernel is
// inflated straight to its load address, and the machine is handed over the way
// ABL hands over to a kernel - caches cleaned, MMU and caches off, x0 =
// devicetree.
//
// The only thing taken on trust is that something branches to our entry point.

#include "gunzip.c"

// ---------------------------------------------------------------- geometry
//
// Fixed addresses, all inside usable RAM from the board devicetree:
// 0xa5a00000 + 0x2f423000 is the largest run (757 MiB), so the ~53 MiB kernel
// fits there with the initramfs and the tree after it.
#define KERNEL_ADDR	0xa5a00000UL		/* 2 MiB aligned, as required */
#define INITRD_ADDR	0xa9400000UL
#define DTB_ADDR	0xa9580000UL

#define BLOB_MAGIC	"NX809JBL"

struct blob_hdr {
	char magic[8];
	u32 kernel_len;
	u32 dtb_len;
	u32 initrd_len;
	u32 reserved;
	u64 kernel_off;
	u64 dtb_off;
	u64 initrd_off;
};

// ---------------------------------------------------------------- self-location
//
// The PE wrapper that builds this image fills three values in: the RVA of the
// blob section, and this marker's own offset inside the flat image. From those
// plus the runtime address of the marker we get both the image base and the
// blobs without asking anyone.

volatile u64 bl_info[4] __attribute__((used)) = {
	0x4e583830394a424cUL,	/* "NX809JBL" - the wrapper searches for this */
	0,			/* [1] RVA of the blob header, patched in */
	0,			/* [2] offset of bl_info in the image, patched in */
	0,			/* [3] spare */
};

// ---------------------------------------------------------------- framebuffer
//
// Progress signal that survives a silent hang and uses no firmware at all: the
// ABL leaves the DPU scanning the splash buffer, so writing pixels shows up on
// the panel even though nothing here talks to a console. Colours: blue = we are
// running, red = kernel inflated, white = about to jump.

#define FB_ADDR		0xfc800000UL
#define FB_PIXELS	(1216 * 2688)

#define FB_BLUE		0xff0000ff
#define FB_RED		0xffff0000
#define FB_WHITE	0xffffffff

static void paint(u32 colour)
{
	volatile u32 *fb = (volatile u32 *)FB_ADDR;
	usize i;

	for (i = 0; i < FB_PIXELS; i++)
		fb[i] = colour;
	__asm__ volatile("dsb sy" ::: "memory");
}

// ---------------------------------------------------------------- cache / MMU

static void clean_dcache(u64 start, u64 end)
{
	u64 p;

	__asm__ volatile("dsb sy" ::: "memory");
	for (p = start & ~63UL; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("ic ialluis" ::: "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}

static void drop_and_jump(u64 dtb, u64 entry) __attribute__((noreturn));

static void drop_and_jump(u64 dtb, u64 entry)
{
	u64 el;
	u64 mask = ~((1UL << 0) | (1UL << 2) | (1UL << 12));	/* M | C | I */

	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	if (el == (2UL << 2)) {
		u64 v;
		__asm__ volatile("mrs %0, sctlr_el2" : "=r"(v));
		v &= mask;
		__asm__ volatile("msr sctlr_el2, %0" :: "r"(v));
	} else {
		u64 v;
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
		v &= mask;
		__asm__ volatile("msr sctlr_el1, %0" :: "r"(v));
	}
	__asm__ volatile("isb" ::: "memory");

	// The arm64 boot protocol: x0 = devicetree, x1/x2/x3 are reserved and must
	// be zero. A kernel entered with garbage in them stops dead before it gets
	// a console up, which is what "stuck at Booting" looked like.
	__asm__ volatile("mov x0, %0" :: "r"(dtb));
	__asm__ volatile("mov x1, xzr" ::: "x1");
	__asm__ volatile("mov x2, xzr" ::: "x2");
	__asm__ volatile("mov x3, xzr" ::: "x3");
	__asm__ volatile("br %0" :: "r"(entry));
	__builtin_unreachable();
}

// ---------------------------------------------------------------- entry point

void efi_main(void) __attribute__((section(".text.start")));

void efi_main(void)
{
	u64 self = (u64)(usize)bl_info;		/* runtime address of the marker */
	u8 *base = (u8 *)(usize)(self - bl_info[2]);
	struct blob_hdr *h;
	u8 *blobs;
	usize out_len = 0;
	usize i;

	paint(FB_BLUE);

	blobs = base + bl_info[1];
	h = (struct blob_hdr *)blobs;

	for (i = 0; i < 8; i++)
		if (h->magic[i] != BLOB_MAGIC[i])
			goto hang;		/* wrapper did not patch us */

	/* 1. devicetree and initramfs: straight copies to fixed addresses */
	{
		const u8 *src = blobs + h->dtb_off;
		u8 *dst = (u8 *)DTB_ADDR;

		for (i = 0; i < h->dtb_len; i++)
			dst[i] = src[i];
	}
	if (h->initrd_len) {
		const u8 *src = blobs + h->initrd_off;
		u8 *dst = (u8 *)INITRD_ADDR;

		for (i = 0; i < h->initrd_len; i++)
			dst[i] = src[i];
	}

	/* 2. kernel: inflate Image.gz straight to its load address */
	if (gunzip(blobs + h->kernel_off, h->kernel_len, (u8 *)KERNEL_ADDR,
		   64UL * 1024 * 1024, &out_len))
		goto hang;

	paint(FB_RED);

	/* 3. make it visible to a kernel that will run with the caches off */
	clean_dcache(KERNEL_ADDR, KERNEL_ADDR + out_len);
	clean_dcache(DTB_ADDR, DTB_ADDR + h->dtb_len);
	if (h->initrd_len)
		clean_dcache(INITRD_ADDR, INITRD_ADDR + h->initrd_len);

	paint(FB_WHITE);

	/* 4. hand over exactly like a bootloader hands over to a kernel */
	drop_and_jump(DTB_ADDR, KERNEL_ADDR);

hang:
	for (;;)
		__asm__ volatile("wfe");
}
