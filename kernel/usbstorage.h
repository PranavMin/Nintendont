#ifndef __USBSTORAGE_H__
#define __USBSTORAGE_H__

#define	USBSTORAGE_OK			0
#define	USBSTORAGE_ENOINTERFACE		-10000
#define	USBSTORAGE_ESENSE		-10001
#define	USBSTORAGE_ESHORTWRITE		-10002
#define	USBSTORAGE_ESHORTREAD		-10003
#define	USBSTORAGE_ESIGNATURE		-10004
#define	USBSTORAGE_ETAG			-10005
#define	USBSTORAGE_ESTATUS		-10006
#define	USBSTORAGE_EDATARESIDUE		-10007
#define	USBSTORAGE_ETIMEDOUT		-10008
#define	USBSTORAGE_EINIT		-10009
#define USBSTORAGE_PROCESSING	-10010

#define B_RAW_DEVICE_DATA_IN 0x01
#define B_RAW_DEVICE_COMMAND 0

typedef struct {
   uint8_t         command[16];
   uint8_t         command_length;
   uint8_t         flags;
   uint8_t         scsi_status;
   void*           data;
   size_t          data_length;
} raw_device_command;

s32 USBStorage_Startup(bool hotswap);
bool USBStorage_ReadSectors(u32 sector, u32 numSectors, void *buffer);
bool USBStorage_WriteSectors(u32 sector, u32 numSectors, const void *buffer);
void USBStorage_Shutdown(void);

void USBStorage_UpdateRegisters_MainThread(void);
bool USBStorage_IsInserted_SlippiThread(void);

/* LazyTO beamer mailbox (RelayEXI.c, relay thread only). USBStorage_Mount
 * returns the mounted drive's id (0 = nothing mounted; a new id per mount, so a
 * re-inserted or swapped drive is a different one) and its sector size. The
 * Read/WriteMounted calls run the cycle only while that same mount is still
 * there. All three take the USB lock (usbstorage.c). */
u32 USBStorage_Mount(u32 *sector_size);
bool USBStorage_ReadMounted(u32 mount, u32 sector, u32 numSectors, void *buffer);
bool USBStorage_WriteMounted(u32 mount, u32 sector, u32 numSectors, const void *buffer);

#endif /* __USBSTORAGE_H__ */
