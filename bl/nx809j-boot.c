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
#define KERNEL_WINDOW	0x3800000UL	/* 56 MiB, the most the kernel can be */

static u64 KERNEL_OUT;
static u64 DTB_OUT;

#define BLOB_MAGIC	"NX809JBL"

struct blob_hdr {
	char magic[8];
	u32 kernel_len;
	u32 dtb_len;
	u32 initrd_len;
	u32 img_size;		/* total size of this PE image */
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

#define FB_BLUE		0xff0000ff	/* entered our entry point */
#define FB_GREEN	0xff00ff00	/* MMU and caches off */
#define FB_CYAN		0xff00ffff	/* blobs located */
#define FB_YELLOW	0xffffff00	/* dtb + initramfs copied */
#define FB_RED		0xffff0000	/* kernel inflated */
#define FB_WHITE	0xffffffff	/* about to jump */

/* Small patch, cheap enough to call while a 52 MiB kernel is inflating. */
static void paint_patch(u32 x, u32 y, u32 w, u32 h, u32 colour)
{
	volatile u32 *fb = (volatile u32 *)FB_ADDR;
	u32 row, col;

	for (row = 0; row < h; row++)
		for (col = 0; col < w; col++)
			fb[(y + row) * 1216 + x + col] = colour;
}

/* Progress: one full-height band per MiB of kernel produced, so it fills the
 * whole panel as the kernel inflates and cannot be missed. */
static void progress(usize produced)
{
	u32 mb = (u32)(produced >> 20);

	if (mb < 53)
		paint_patch(mb * 22, 0, 20, 2688, FB_WHITE);
}

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

// Turn the MMU, D-cache and I-cache off for the current exception level.
//
// This runs *first*, before touching any memory outside our own image: the
// firmware's page tables only cover what it happens to use, and writing the
// kernel to 0xa5a00000 through them faulted (blue screen, then nothing). With
// the MMU off every access is a plain physical one, which is also exactly the
// state a kernel wants to be entered in.
static void mmu_caches_off(void)
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
}

static void drop_and_jump(u64 dtb, u64 entry) __attribute__((noreturn));

static void drop_and_jump(u64 dtb, u64 entry)
{

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

	mmu_caches_off();
	paint(FB_GREEN);

	blobs = base + bl_info[1];
	h = (struct blob_hdr *)blobs;

	for (i = 0; i < 8; i++)
		if (h->magic[i] != BLOB_MAGIC[i])
			goto hang;		/* wrapper did not patch us */

	paint(FB_CYAN);

	/* 1. decide where the payload goes.
	 *
	 * The fixed addresses below are what the devicetree was baked with, but
	 * the loader may have put *us* right on top of them - copying the tree
	 * over our own code mid-inflate hangs the machine (which is what a tree
	 * 229 bytes larger suddenly did). So if our image overlaps the kernel
	 * window, put the whole payload above ourselves instead, and patch the
	 * initramfs addresses in the tree to match.
	 */
	{
		u64 self_lo = (u64)(usize)base;
		u64 self_hi = self_lo + (u64)h->img_size;
		u64 kaddr = KERNEL_ADDR;
		u64 iaddr = INITRD_ADDR;
		u64 daddr = DTB_ADDR;

		if (self_hi > kaddr && self_lo < kaddr + KERNEL_WINDOW) {
			kaddr = (self_hi + 0x1fffffUL) & ~0x1fffffUL;
			iaddr = kaddr + KERNEL_WINDOW;
			daddr = iaddr + 0x200000UL;
		}

		/* copy the tree first, then fix its initramfs range if we moved */
		{
			const u8 *src = blobs + h->dtb_off;
			u8 *dst = (u8 *)daddr;

			for (i = 0; i < h->dtb_len; i++)
				dst[i] = src[i];
		}
		if (iaddr != INITRD_ADDR) {
			u32 old_s[2] = { (u32)(INITRD_ADDR >> 32), (u32)INITRD_ADDR };
			u32 old_e[2] = { (u32)((INITRD_ADDR + h->initrd_len) >> 32),
					 (u32)(INITRD_ADDR + h->initrd_len) };
			u32 new_s[2] = { (u32)(iaddr >> 32), (u32)iaddr };
			u32 new_e[2] = { (u32)((iaddr + h->initrd_len) >> 32),
					 (u32)(iaddr + h->initrd_len) };
			usize j;

			for (j = 0; j + 8 <= h->dtb_len; j += 4) {
				u8 *q = (u8 *)daddr + j;

				if (q[0] == (u8)(old_s[0] >> 24) && q[1] == (u8)(old_s[0] >> 16) &&
				    q[2] == (u8)(old_s[0] >> 8) && q[3] == (u8)old_s[0] &&
				    q[4] == (u8)(old_s[1] >> 24) && q[5] == (u8)(old_s[1] >> 16) &&
				    q[6] == (u8)(old_s[1] >> 8) && q[7] == (u8)old_s[1]) {
					u32 v;
					for (v = 0; v < 4; v++)
						q[v] = (u8)(new_s[v >> 2] >> (24 - 8 * (v & 3)));
				} else if (q[0] == (u8)(old_e[0] >> 24) && q[1] == (u8)(old_e[0] >> 16) &&
					   q[2] == (u8)(old_e[0] >> 8) && q[3] == (u8)old_e[0] &&
					   q[4] == (u8)(old_e[1] >> 24) && q[5] == (u8)(old_e[1] >> 16) &&
					   q[6] == (u8)(old_e[1] >> 8) && q[7] == (u8)old_e[1]) {
					u32 v;
					for (v = 0; v < 4; v++)
						q[v] = (u8)(new_e[v >> 2] >> (24 - 8 * (v & 3)));
				}
			}
		}
		if (h->initrd_len) {
			const u8 *src = blobs + h->initrd_off;
			u8 *dst = (u8 *)iaddr;

			for (i = 0; i < h->initrd_len; i++)
				dst[i] = src[i];
		}

		paint(FB_YELLOW);

		/* remember the addresses for the inflate and the jump */
		KERNEL_OUT = kaddr;
		DTB_OUT = daddr;
	}

	/* 2. kernel: inflate Image.gz straight to its load address */
	gz_progress = progress;
	if (gunzip(blobs + h->kernel_off, h->kernel_len, (u8 *)KERNEL_OUT,
		   64UL * 1024 * 1024, &out_len))
		goto hang;

	paint(FB_RED);

	/* 3. make it visible to a kernel that will run with the caches off */
	clean_dcache(KERNEL_OUT, KERNEL_OUT + out_len);
	clean_dcache(DTB_OUT, DTB_OUT + h->dtb_len);
	if (h->initrd_len)
		clean_dcache(KERNEL_OUT + KERNEL_WINDOW,
			     KERNEL_OUT + KERNEL_WINDOW + h->initrd_len);

	paint(FB_WHITE);

	/* 4. hand over exactly like a bootloader hands over to a kernel */
	drop_and_jump(DTB_OUT, KERNEL_OUT);

hang:
	for (;;)
		__asm__ volatile("wfe");
}
