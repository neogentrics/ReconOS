/* UEFI, declared from the specification rather than borrowed.
 *
 * This is normally where a project links gnu-efi. We do not, for the same
 * reason the kernel does not link a libc: the whole claim is that it was built.
 * What is here is only what the loader actually calls -- perhaps a twentieth of
 * the interface -- and the rest is declared as opaque pointers.
 *
 * THAT IS THE ONE THING TO BE CAREFUL ABOUT. UEFI tables are an ABI: the
 * firmware fills in a structure and we index into it. A missing field does not
 * fail to compile, it shifts every field after it, and the call lands on the
 * wrong function pointer. So every table below lists ITS FULL SET OF MEMBERS IN
 * ORDER, with the ones we do not use typed as `void *` placeholders rather than
 * omitted. Deleting an unused placeholder to tidy up would be a subtle and
 * complete disaster.
 *
 * Calling convention: UEFI uses the Microsoft x64 ABI on x86_64 and AAPCS64 on
 * aarch64. Building with `clang -target <arch>-unknown-windows` gives both,
 * which is why the loader is compiled by clang and the kernel by gcc.
 */
#ifndef RECON_EFI_H
#define RECON_EFI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int64_t   INTN;
typedef uint64_t  UINTN;		/* 64-bit: this loader is 64-bit only */
typedef uint16_t  CHAR16;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef UINTN     EFI_STATUS;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            0x8000000000000001ULL
#define EFI_INVALID_PARAMETER     0x8000000000000002ULL
#define EFI_UNSUPPORTED           0x8000000000000003ULL
#define EFI_BUFFER_TOO_SMALL      0x8000000000000005ULL
#define EFI_NOT_FOUND             0x800000000000000EULL

#define EFI_ERROR(s) (((EFI_STATUS)(s)) & 0x8000000000000000ULL)

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8  Data4[8];
} EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

/* --- Memory ------------------------------------------------------------- */

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

typedef struct {
	UINT32 Type;
	UINT32 Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS  VirtualStart;
	UINT64 NumberOfPages;
	UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* --- Simple text output ------------------------------------------------- */

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	void *Reset;
	EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
					  CHAR16 *string);
	void *TestString;
	void *QueryMode;
	void *SetMode;
	EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
					  UINTN attribute);
	EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self);
	void *SetCursorPosition;
	void *EnableCursor;
	void *Mode;
};

/* --- Graphics Output ---------------------------------------------------- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{ 0x9042a9de, 0x23dc, 0x4a38, \
	  { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN  SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *self,
				       UINT32 mode, UINTN *size_of_info,
				       EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
	EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *self, UINT32 mode);
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* --- Loaded image ------------------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{ 0x5b1b31a1, 0x9562, 0x11d2, \
	  { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	void *SystemTable;
	EFI_HANDLE DeviceHandle;	/* the volume we were loaded from */
	void *FilePath;
	void *Reserved;
	UINT32 LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	UINT64 ImageSize;
	EFI_MEMORY_TYPE ImageCodeType;
	EFI_MEMORY_TYPE ImageDataType;
	void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* --- Files -------------------------------------------------------------- */

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{ 0x964e5b22, 0x6459, 0x11d2, \
	  { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_GUID \
	{ 0x09576e92, 0x6d3f, 0x11d2, \
	  { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct _EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **out,
				  CHAR16 *name, UINT64 mode, UINT64 attributes);
	EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *self);
	void *Delete;
	EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *self, UINTN *size, void *buf);
	void *Write;
	void *GetPosition;
	EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *self, UINT64 position);
	EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *self, EFI_GUID *type,
				     UINTN *size, void *buf);
	void *SetInfo;
	void *Flush;
};

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,
					EFI_FILE_PROTOCOL **root);
};

typedef struct {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	UINT8  CreateTime[16];
	UINT8  LastAccessTime[16];
	UINT8  ModificationTime[16];
	UINT64 Attribute;
	CHAR16 FileName[1];
} EFI_FILE_INFO;

/* --- Boot services ------------------------------------------------------
 *
 * Every member, in order. The `void *` entries are functions this loader does
 * not call; they are here to hold the offsets of the ones it does.
 */

typedef struct {
	EFI_TABLE_HEADER Hdr;

	void *RaiseTPL;
	void *RestoreTPL;

	EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE type,
					   EFI_MEMORY_TYPE memory_type,
					   UINTN pages,
					   EFI_PHYSICAL_ADDRESS *memory);
	EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS memory, UINTN pages);
	EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *map_size,
					  EFI_MEMORY_DESCRIPTOR *map,
					  UINTN *map_key,
					  UINTN *descriptor_size,
					  UINT32 *descriptor_version);
	EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE type, UINTN size,
					  void **buffer);
	EFI_STATUS (EFIAPI *FreePool)(void *buffer);

	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;

	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE handle, EFI_GUID *protocol,
					    void **interface);
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;

	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE image_handle, UINTN map_key);

	void *GetNextMonotonicCount;
	EFI_STATUS (EFIAPI *Stall)(UINTN microseconds);
	void *SetWatchdogTimer;

	void *ConnectController;
	void *DisconnectController;

	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;

	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *protocol, void *registration,
					    void **interface);
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;

	void *CalculateCrc32;

	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
	EFI_GUID VendorGuid;
	void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* The ACPI root pointer is not a protocol -- it is an entry in the
 * configuration table, found by matching its GUID. */
#define EFI_ACPI_20_TABLE_GUID \
	{ 0x8868e871, 0xe4f1, 0x11d3, \
	  { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define EFI_ACPI_10_TABLE_GUID \
	{ 0xeb9d2d30, 0x2d88, 0x11d3, \
	  { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* The device tree, where firmware provides one alongside UEFI -- common on
 * aarch64 boards, where ACPI is the exception rather than the rule. */
#define EFI_DTB_TABLE_GUID \
	{ 0xb1b621d5, 0xf19c, 0x41a5, \
	  { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }

#endif /* RECON_EFI_H */
