// SPDX-License-Identifier: GPL-2.0
//
// Minimal EFI bootloader for the Nubia RedMagic 11S Pro+ (NX809J, SM8850
// "kaanapali") that boots a mainline arm64 Linux kernel.
//
// Why this exists: on this board the stock ABL verifies everything it boots
// against the OEM vbmeta (a one-byte change to boot_a is enough to have the
// slot marked unbootable), so a mainline kernel cannot be put in boot_a. The
// only unverified executable slot is the `efisp` partition: the first stage
// XBL starts whatever UEFI application lives there, and that program ("GBL",
// a boot manager reading an ESP out of `persist`) is what shows the boot menu.
// Placing *this* file in that ESP adds a menu entry that boots mainline Linux.
//
// It does the same job the ABL does when it hands control to a kernel:
//   1. copy kernel/dtb/initramfs to their load addresses (ABL uses fixed
//      addresses out of the usable RAM map too),
//   2. leave EFI (ExitBootServices),
//   3. clean the caches and turn the MMU and caches off for the current EL,
//   4. branch to the kernel with x0 = devicetree, as the arm64 boot protocol
//      requires.
//
// The devicetree is pre-baked by the build (its /chosen carries the cmdline
// and the initramfs addresses), so nothing here has to parse or patch FDTs.
//
// Build: see .github/workflows/bl.yml (freestanding, position independent:
// no relocations, so the resulting raw binary can be wrapped as a PE image
// with a valid-but-empty .reloc section, which is what GBL's loader wants).

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long usize;

#define EFIAPI
#define NULL ((void *)0)

// ---------------------------------------------------------------- EFI types

typedef struct {
	u32 Data1;
	u16 Data2;
	u16 Data3;
	u8 Data4[8];
} EFI_GUID;

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u64 EFI_PHYSICAL_ADDRESS;
typedef u64 EFI_VIRTUAL_ADDRESS;
typedef u64 EFI_LBA;
typedef usize UINTN;

#define EFI_SUCCESS 0

typedef struct {
	u64 Signature;
	u32 Revision;
	u32 HeaderSize;
	u32 CRC32;
	u32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
	u32 Type;
	u32 Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS VirtualStart;
	u64 NumberOfPages;
	u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiMaxMemoryType
} EFI_MEMORY_TYPE;

// Only the fields we actually use are named; the order is what matters, so
// the remaining slots are declared as reserved pointers of the right size.
typedef struct EFI_BOOT_SERVICES {
	EFI_TABLE_HEADER Hdr;                                   // 0
	void *RaiseTPL;                                         // 24
	void *RestoreTPL;                                       // 32
	EFI_STATUS(EFIAPI * AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE,
					   UINTN, EFI_PHYSICAL_ADDRESS *); // 40
	void *FreePages;                                        // 48
	EFI_STATUS(EFIAPI * GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *,
					  UINTN *, UINTN *, u32 *);     // 56
	EFI_STATUS(EFIAPI * AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **); // 64
	void *FreePool;                                         // 72
	void *CreateEvent;                                      // 80
	void *SetTimer;                                         // 88
	void *WaitForEvent;                                     // 96
	void *SignalEvent;                                      // 104
	void *CloseEvent;                                       // 112
	void *CheckEvent;                                       // 120
	void *InstallProtocolInterface;                         // 128
	void *ReinstallProtocolInterface;                       // 136
	void *UninstallProtocolInterface;                       // 144
	EFI_STATUS(EFIAPI * HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **); // 152
	void *Reserved;                                         // 160
	void *RegisterProtocolNotify;                           // 168
	void *LocateHandle;                                     // 176
	void *LocateDevicePath;                                 // 184
	void *InstallConfigurationTable;                        // 192
	EFI_STATUS(EFIAPI * LoadImage)(u8, EFI_HANDLE, void *, void *, UINTN,
					EFI_HANDLE *);                 // 200
	EFI_STATUS(EFIAPI * StartImage)(EFI_HANDLE, UINTN *, u16 **); // 208
	void *Exit;                                             // 216
	void *UnloadImage;                                      // 224
	EFI_STATUS(EFIAPI * ExitBootServices)(EFI_HANDLE, UINTN); // 232
	void *GetNextMonotonicCount;                            // 240
	EFI_STATUS(EFIAPI * Stall)(UINTN);                      // 248
	void *SetWatchdogTimer;                                 // 256
	void *ConnectController;                                // 264
	void *DisconnectController;                             // 272
	void *OpenProtocol;                                     // 280
	void *CloseProtocol;                                    // 288
	void *OpenProtocolInformation;                          // 296
	void *ProtocolsPerHandle;                               // 304
	EFI_STATUS(EFIAPI * LocateHandleBuffer)(int, EFI_GUID *, void *,
						UINTN *, EFI_HANDLE **); // 312
	void *LocateProtocol;                                   // 320
} EFI_BOOT_SERVICES;

typedef struct {
	u32 Data1;
	u16 Data2;
	u16 Data3;
	u8 Data4[8];
} EFI_CONFIGURATION_TABLE;

typedef struct EFI_SYSTEM_TABLE {
	EFI_TABLE_HEADER Hdr;                                   // 0
	u16 *FirmwareVendor;                                    // 24
	u32 FirmwareRevision;                                   // 32 + pad
	void *ConsoleInHandle;                                  // 40
	void *ConIn;                                            // 48
	void *ConsoleOutHandle;                                 // 56
	void *ConOut;                                           // 64
	void *StandardErrorHandle;                              // 72
	void *StdErr;                                           // 80
	void *RuntimeServices;                                  // 88
	EFI_BOOT_SERVICES *BootServices;                        // 96
	UINTN NumberOfTableEntries;                             // 104
	EFI_CONFIGURATION_TABLE *ConfigurationTable;            // 112
} EFI_SYSTEM_TABLE;

typedef struct {
	u32 MediaId;
	u8 RemovableMedia;
	u8 MediaPresent;
	u8 LogicalPartition;
	u8 ReadOnly;
	u8 WriteCaching;
	u8 Pad[3];
	u32 BlockSize;
	u32 IoAlign;
	EFI_LBA LastBlock;
	EFI_LBA LowestAlignedLba;
	u32 LogicalBlocksPerPhysicalBlock;
	u32 OptimalTransferLengthGranularity;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
	u64 Revision;
	EFI_BLOCK_IO_MEDIA *Media;
	EFI_STATUS(EFIAPI * Reset)(void *, int);
	EFI_STATUS(EFIAPI * ReadBlocks)(void *, u32, EFI_LBA, UINTN, void *);
	EFI_STATUS(EFIAPI * WriteBlocks)(void *, u32, EFI_LBA, UINTN, void *);
	void *FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

static const EFI_GUID gBlockIoGuid = { 0x964e5b21, 0x6459, 0x11d2,
				 { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

// ---------------------------------------------------------------- blob layout
//
// `recovery_a` holds, at offset BLOB_OFF, a header followed by the kernel
// Image, the pre-baked devicetree and the initramfs, all concatenated. The
// offset is well past the partition's GPT/secondary-GPT area and gives the
// bootloader a fixed place to look without needing a filesystem driver.

#define BLOB_OFF	0x100000UL
#define BLOB_MAGIC	"NX809JBL"		/* 8 bytes, incl. NUL */
#define KERNEL_ADDR	0xa5a00000UL		/* 2 MiB aligned, inside the
						 * 0xa5a00000+0x2f423000 usable
						 * range from the board DTS */
#define INITRD_ADDR	0xa9400000UL		/* after the ~53 MiB kernel */
#define DTB_ADDR	0xa9580000UL		/* after the initramfs */

struct blob_hdr {
	char magic[8];		/* BLOB_MAGIC */
	u32 kernel_len;
	u32 dtb_len;
	u32 initrd_len;
	u32 reserved;
	u64 kernel_off;		/* all offsets relative to BLOB_OFF */
	u64 dtb_off;
	u64 initrd_off;
};

// ---------------------------------------------------------------- tiny runtime

static void *memcpy_(void *dst, const void *src, usize n)
{
	u8 *d = dst;
	const u8 *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}


static int memeq(const void *a, const void *b, usize n)
{
	const u8 *x = a, *y = b;
	while (n--)
		if (*x++ != *y++)
			return 0;
	return 1;
}

// ---------------------------------------------------------------- cache / MMU

static void clean_dcache(u64 start, u64 end)
{
	// Clean and invalidate by VA to the point of coherency. CTR_EL0 tells us
	// the line size; 64 (the architectural maximum for the D-cache on this
	// SoC) is fine for the loop stride, we just clean a little extra.
	u64 p;
	__asm__ volatile("dsb sy" ::: "memory");
	for (p = start & ~63UL; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("ic ialluis" ::: "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	__asm__ volatile("isb" ::: "memory");
}

static void drop_to_el_and_jump(u64 dtb, u64 entry) __attribute__((noreturn));

static void drop_to_el_and_jump(u64 dtb, u64 entry)
{
	// Turn the MMU and both caches off for the current exception level. The
	// kernel's boot protocol wants them off, and here it also has the side
	// effect we need: EDK2 maps a loaded image's .text read-only and .data
	// non-executable, neither of which a bare kernel image tolerates.
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

	__asm__ volatile("mov x0, %0" :: "r"(dtb));
	__asm__ volatile("br %0" :: "r"(entry));
	__builtin_unreachable();
}

// ---------------------------------------------------------------- block I/O

// Read `len` bytes at byte offset `off` of a block device into `dst`.
static EFI_STATUS read_at(EFI_BLOCK_IO_PROTOCOL *bio, u64 off, u64 len, void *dst)
{
	u32 bs = bio->Media->BlockSize;
	EFI_STATUS st;
	u8 *p = dst;

	while (len) {
		u64 blk = off / bs;
		u64 skip = off % bs;
		u64 chunk = (bs > 8192) ? 8192 : bs;
		u8 bounce[8192];
		u64 n;

		if (skip) {
			// Unaligned read: pull one block and copy the tail out of it.
			st = bio->ReadBlocks(bio, bio->Media->MediaId, blk, bs, bounce);
			if (st != EFI_SUCCESS)
				return st;
			n = bs - skip;
			if (n > len)
				n = len;
			memcpy_(p, bounce + skip, n);
			p += n;
			off += n;
			len -= n;
			continue;
		}

		chunk = (len / bs);
		if (chunk > 16)		/* 16 * 4 KiB = 64 KiB per call */
			chunk = 16;
		if (chunk == 0)
			chunk = 1;
		if (chunk * bs > len)
			chunk = len / bs;
		if (chunk == 0) {
			// Less than a block left: read one and take the head.
			st = bio->ReadBlocks(bio, bio->Media->MediaId, blk, bs, bounce);
			if (st != EFI_SUCCESS)
				return st;
			memcpy_(p, bounce, len);
			return EFI_SUCCESS;
		}
		st = bio->ReadBlocks(bio, bio->Media->MediaId, blk, chunk * bs, p);
		if (st != EFI_SUCCESS)
			return st;
		n = chunk * bs;
		p += n;
		off += n;
		len -= n;
	}
	return EFI_SUCCESS;
}

// ------------------------------------------------------------ console / input

static EFI_SYSTEM_TABLE *sys_table;

typedef struct {
	u16 ScanCode;
	u16 UnicodeChar;
} EFI_INPUT_KEY;

typedef struct {
	void *Reset;
	EFI_STATUS(EFIAPI * ReadKeyStroke)(void *, EFI_INPUT_KEY *);
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
	void *Reset;
	EFI_STATUS(EFIAPI * OutputString)(void *, u16 *);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *conOut;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conIn;
static u16 console_buf[256];


static void print(const char *s)
{
	u16 *out = console_buf;
	usize n = 0;

	while (*s && n < 254) {
		if (*s == '\n')
			out[n++] = '\r';
		out[n++] = (u16)(u8)*s++;
	}
	out[n] = 0;
	conOut->OutputString(conOut, console_buf);
}


// Poll the console for a key for up to `ms` milliseconds. Returns the
// unicode character, or 0 if nothing was pressed. Uses Stall() because the
// firmware timer protocols are not worth depending on here.
static u16 wait_key(u32 ms)
{
	EFI_BOOT_SERVICES *bs = sys_table->BootServices;
	u32 waited = 0;

	while (waited < ms) {
		EFI_INPUT_KEY key;
		if (conIn && conIn->ReadKeyStroke(conIn, &key) == EFI_SUCCESS)
			return key.UnicodeChar ? key.UnicodeChar : 0x100 + key.ScanCode;
		bs->Stall(20000);	/* 20 ms */
		waited += 20;
	}
	return 0;
}

// ---------------------------------------------------------------- chainload
//
// Booting Android from here means starting the very UEFI application GBL would
// have started for its own 'Android' entry: the stock loader in the ESP. That
// is a LoadImage/StartImage of a file on the volume we were started from, so we
// build a device path by appending a file-path node to our own device path.

typedef struct {
	u32 Revision;
	u32 Pad;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct {
	u64 Revision;
	EFI_STATUS(EFIAPI * Open)(void *, void **, u16 *, u64, u64);
	EFI_STATUS(EFIAPI * Close)(void *);
	void *Delete;
	void *Read;
	void *Write;
	void *GetPosition;
	void *SetPosition;
	void *GetInfo;
	void *SetInfo;
	void *Flush;
} EFI_FILE_PROTOCOL;

typedef struct {
	u64 Revision;
	EFI_STATUS(EFIAPI * OpenVolume)(void *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#pragma pack(1)
typedef struct {
	u8 Type;
	u8 SubType;
	u16 Length;
} EFI_DEVICE_PATH_PROTOCOL;
#pragma pack()

static const EFI_GUID gLoadedImageGuid = { 0x5b1b31a1, 0x9562, 0x11d2,
					   { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const EFI_GUID gSimpleFsGuid = { 0x964e5b22, 0x6459, 0x11d2,
					{ 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static void ascii_to_u16(const char *s, u16 *out)
{
	while (*s)
		*out++ = (u16)(u8)*s++;
	*out = 0;
}

// Boot `path` (ASCII, e.g. "\\efisp\\boot_android.efi") off our own volume.
static EFI_STATUS chainload(EFI_HANDLE image_handle, const char *path)
{
	EFI_BOOT_SERVICES *bs = sys_table->BootServices;
	EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
	EFI_FILE_PROTOCOL *root = NULL, *file = NULL;
	EFI_DEVICE_PATH_PROTOCOL *devpath = NULL;
	EFI_HANDLE new_image = NULL;
	u8 pathbuf[256];
	EFI_DEVICE_PATH_PROTOCOL *fp = (EFI_DEVICE_PATH_PROTOCOL *)pathbuf;
	u16 name[128];
	EFI_STATUS status;
	usize plen, i;

	status = bs->HandleProtocol(image_handle, (EFI_GUID *)&gLoadedImageGuid,
				    (void **)&li);
	if (status != EFI_SUCCESS || !li)
		return status;
	status = bs->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&gSimpleFsGuid,
				    (void **)&fs);
	if (status != EFI_SUCCESS || !fs)
		return status;
	status = fs->OpenVolume(fs, &root);
	if (status != EFI_SUCCESS)
		return status;

	ascii_to_u16(path, name);
	status = root->Open(root, (void **)&file, name, 1 /* read */, 0);
	if (status != EFI_SUCCESS)
		return status;

	// device path: our own, with a MEDIA/FILEPATH node appended
	status = bs->HandleProtocol(li->DeviceHandle,
				    (EFI_GUID *)&(EFI_GUID){ 0x09576e91, 0x6d3f, 0x11d2,
						{ 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } },
				    (void **)&devpath);
	if (status != EFI_SUCCESS || !devpath)
		return status;
	plen = 0;
	while (plen < 16 && devpath[plen].Length &&
	       !(devpath[plen].Type == 0x7f && devpath[plen].SubType == 0xff))
		plen++;
	{
		usize dlen = 0;
		for (i = 0; i < plen; i++)
			dlen += devpath[i].Length;
		if (dlen + 4 + 2 * 128 + 4 > sizeof(pathbuf))
			return 1;
		{
			u8 *dst = pathbuf;
			for (i = 0; i < plen; i++) {
				usize k;
				u8 *src = (u8 *)&devpath[i];
				for (k = 0; k < devpath[i].Length; k++)
					*dst++ = src[k];
			}
			fp = (EFI_DEVICE_PATH_PROTOCOL *)dst;
		}
	}
	fp->Type = 4;		/* MEDIA_DEVICE_PATH */
	fp->SubType = 4;	/* MEDIA_FILEPATH_DP */
	{
		u16 *fname = (u16 *)((u8 *)fp + 4);
		usize n = 0;
		ascii_to_u16(path, fname);
		while (fname[n])
			n++;
		fp->Length = (u16)(4 + 2 * (n + 1));
		/* end-of-path node */
		{
			EFI_DEVICE_PATH_PROTOCOL *end =
				(EFI_DEVICE_PATH_PROTOCOL *)((u8 *)fp + fp->Length);
			end->Type = 0x7f;
			end->SubType = 0xff;
			end->Length = 4;
		}
	}

	status = bs->LoadImage(0 /* not boot policy */, image_handle,
			       (void *)pathbuf, NULL, 0, &new_image);
	if (status != EFI_SUCCESS)
		return status;
	return bs->StartImage(new_image, NULL, NULL);
}

// ---------------------------------------------------------------- progress
//
// The vendor memory-dump screen overwrites the panel, so anything printed
// before a crash is lost. Each stage therefore stamps a marker block in the
// partition that holds the payload (block 255, i.e. just before BLOB_OFF); it
// can be read back afterwards with
//     dd if=/dev/block/by-name/recovery_a bs=4096 skip=255 count=1 | xxd
// which is how we tell "the scan crashed" from "the kernel would not start".

#define MARK_BLOCK	((BLOB_OFF / 4096) - 1)
#define MARK_MAGIC	"NX809JMK"

static void mark(EFI_BLOCK_IO_PROTOCOL *bio, u8 code)
{
	u8 buf[4096];

	if (!bio || bio->Media->BlockSize != 4096)
		return;
	memcpy_(buf, MARK_MAGIC, 8);
	buf[8] = code;
	bio->WriteBlocks(bio, bio->Media->MediaId, MARK_BLOCK, 4096, buf);
}

// ---------------------------------------------------------------- entry point

void efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
	__attribute__((section(".text.start")));

void efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
	EFI_BOOT_SERVICES *bs = st->BootServices;
	EFI_HANDLE *handles = NULL;
	UINTN nhandles = 0;
	struct blob_hdr hdr;
	EFI_STATUS status;
	UINTN i;
	int found = 0;
	int boot_linux = 1;
	EFI_BLOCK_IO_PROTOCOL *blob_bio = NULL;

	sys_table = st;
	conOut = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)st->ConOut;
	conIn = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)st->ConIn;

	// 1. find the block device holding our payload
	status = bs->LocateHandleBuffer(2 /* ByProtocol */, (EFI_GUID *)&gBlockIoGuid,
					NULL, &nhandles, &handles);
	if (status == EFI_SUCCESS) {
		for (i = 0; i < nhandles; i++) {
			EFI_BLOCK_IO_PROTOCOL *bio = NULL;
			u8 probe[8];

			status = bs->HandleProtocol(handles[i], (EFI_GUID *)&gBlockIoGuid,
						    (void **)&bio);
			if (status != EFI_SUCCESS || !bio || !bio->Media)
				continue;
			if (!bio->Media->MediaPresent || bio->Media->BlockSize == 0)
				continue;
			if (bio->Media->LastBlock * bio->Media->BlockSize <
			    BLOB_OFF + sizeof(hdr))
				continue;
			if (read_at(bio, BLOB_OFF, sizeof(probe), probe) != EFI_SUCCESS)
				continue;
			if (!memeq(probe, BLOB_MAGIC, 8))
				continue;
			if (read_at(bio, BLOB_OFF, sizeof(hdr), &hdr) != EFI_SUCCESS)
				continue;
			if (hdr.kernel_len == 0 || hdr.dtb_len == 0)
				continue;

			blob_bio = bio;
			mark(bio, 1);		/* payload found */

			// 2. copy the three blobs to their load addresses
			if (read_at(bio, BLOB_OFF + hdr.kernel_off, hdr.kernel_len,
				    (void *)KERNEL_ADDR) != EFI_SUCCESS)
				continue;
			mark(bio, 2);		/* kernel copied */
			if (read_at(bio, BLOB_OFF + hdr.dtb_off, hdr.dtb_len,
				    (void *)DTB_ADDR) != EFI_SUCCESS)
				continue;
			mark(bio, 3);		/* dtb copied */
			if (hdr.initrd_len &&
			    read_at(bio, BLOB_OFF + hdr.initrd_off, hdr.initrd_len,
				    (void *)INITRD_ADDR) != EFI_SUCCESS)
				continue;
			mark(bio, 4);		/* initrd copied */
			found = 1;
			break;
		}
	}

	if (!found) {
		print("no kernel payload found (recovery_a is missing the blob)"
		      "\nreturning to the boot menu...\n");
		return;
	}

	print("\nNX809J bootloader\n\n  1) Android          "
	      "(boot the stock Android loader)\n"
	      "  2) Linux (mainline) (boot the kernel)\n\n"
	      "  press 1/2 or volume up/down; default Linux in 5 s\n\n");

	// 3. menu: a key press within 5 s decides, otherwise Linux
	for (i = 0; i < 25; i++) {
		u16 key = wait_key(200);
		if (!key)
			continue;
		if (key == '1') {
			boot_linux = 0;
			break;
		}
		if (key == '2') {
			boot_linux = 1;
			break;
		}
		if (key == 0x100 + 0x01 /* up */ || key == 'u') {
			boot_linux = 1;
			break;
		}
		if (key == 0x100 + 0x02 /* down */ || key == 'd') {
			boot_linux = 0;
			break;
		}
	}

	mark(blob_bio, 5);		/* menu answered */

	if (!boot_linux) {
		print("starting Android...\n");
		status = chainload(image_handle, "\\efisp\\boot_android.efi");
		print("could not start Android; returning to the boot menu\n");
		return;
	}

	print("starting Linux...\n");

	mark(blob_bio, 6);		/* last marker: no services past here */

	// 4. leave boot services. GetMemoryMap's key must be the one in effect
	//    when ExitBootServices is called, so retry if the map grew.
	{
		void *mapbuf = NULL;
		UINTN mapsize = 0, mapkey, descsize;
		u32 descver;
		int tries;

		if (bs->AllocatePool(EfiLoaderData, 65536, &mapbuf) != EFI_SUCCESS)
			return;

		for (tries = 0; tries < 4; tries++) {
			mapsize = 65536;
			status = bs->GetMemoryMap(&mapsize,
						  (EFI_MEMORY_DESCRIPTOR *)mapbuf,
						  &mapkey, &descsize, &descver);
			if (status != EFI_SUCCESS)
				break;
			status = bs->ExitBootServices(image_handle, mapkey);
			if (status == EFI_SUCCESS)
				break;
		}
		if (status != EFI_SUCCESS)
			return;
	}

	// 5. publish the payload and jump
	clean_dcache(KERNEL_ADDR, KERNEL_ADDR + hdr.kernel_len);
	clean_dcache(DTB_ADDR, DTB_ADDR + hdr.dtb_len);
	if (hdr.initrd_len)
		clean_dcache(INITRD_ADDR, INITRD_ADDR + hdr.initrd_len);

	drop_to_el_and_jump(DTB_ADDR, KERNEL_ADDR);
}
