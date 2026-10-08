#include "SlippiFileWriter.h"
#include "SlippiMemory.h"
#include "alloc.h"
#include "debug.h"
#include "string.h"
#include "ff_utf8.h"
#include "net.h"

#include "Config.h"
#include "usbstorage.h"
#include "RelayEXI.h"

// use common physical sector size so as to write efficiently
// and not excessively wear out the underlying flash storage
#define READ_BUF_SIZE 4096

#define THREAD_CYCLE_TIME_MS 100
#define THREAD_ERROR_TIME_MS 2000
#define LED_FLASH_TIME_MS 1000

#define FOOTER_BUFFER_LENGTH 200

static u32 SlippiHandlerThread(void *arg);

// Thread stuff
static u32 Slippi_Thread;
extern char __slippi_stack_addr, __slippi_stack_size;

// File writing stuff
extern u8 wifi_mac_address[6]; // Used to identify replays

// File object
FIL currentFile;

// vars for metadata generation
u32 gameStartTime;

// timer for drive led
u32 driveTimer;

// flag for drive led timer
bool driveTimerSet;

// replays LED setting
bool replaysLED;

extern FATFS *devices[2];

void SlippiFileWriterInit(bool led)
{
	replaysLED = led;

	Slippi_Thread = do_thread_create(
		SlippiHandlerThread,
		((u32 *)&__slippi_stack_addr),
		((u32)(&__slippi_stack_size)),
		0x78);
	thread_continue(Slippi_Thread);
}

void SlippiFileWriterUpdateRegisters()
{
	if (driveTimerSet && TimerDiffMs(driveTimer) >= LED_FLASH_TIME_MS)
	{
		clear32(HW_GPIO_OUT, GPIO_SLOT_LED);
		driveTimerSet = false;
	}
}

void SlippiFileWriterShutdown()
{
	thread_cancel(Slippi_Thread, 0);
}

void flashLED()
{
	driveTimer = read32(HW_TIMER);
	if (!driveTimerSet)
	{
		set32(HW_GPIO_OUT, GPIO_SLOT_LED);
		driveTimerSet = true;
	}
}

//we cant include time.h so hardcode what we need
struct tm
{
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
};
extern struct tm *gmtime(u32 *time);

char *generateFileName()
{
	// // Add game start time
	// u8 dateTimeStrLength = sizeof "20171015T095717";
	// char *dateTimeBuf = (char *)malloc(dateTimeStrLength);
	// strftime(&dateTimeBuf[0], dateTimeStrLength, "%Y%m%dT%H%M%S", localtime(&gameStartTime));

	// std::string str(&dateTimeBuf[0]);
	// return StringFromFormat("Slippi/Game_%s.slp", str.c_str());

	static char pathStr[50];
	struct tm *tmp = gmtime(&gameStartTime);

	_sprintf(
		&pathStr[0], "/Slippi/Game_%02X%02X%02X%02X%02X%02X_%04d%02d%02dT%02d%02d%02d.slp",
		wifi_mac_address[0], wifi_mac_address[1], wifi_mac_address[2], wifi_mac_address[3],
		wifi_mac_address[4], wifi_mac_address[5], tmp->tm_year + 1900, tmp->tm_mon + 1,
		tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec);

	return pathStr;
}

FRESULT writeHeader(FIL *file)
{
	u8 header[] = {'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0};

	u32 wrote;
	return f_write(file, header, sizeof(header), &wrote);
}

FRESULT completeFile(FIL *file, s32 lastFrame, u32 writtenByteCount)
{
	u8 footer[FOOTER_BUFFER_LENGTH];
	u32 writePos = 0;

	// Write opener
	u8 footerOpener[] = {'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{'};
	u8 writeLen = sizeof(footerOpener);
	memcpy(&footer[writePos], footerOpener, writeLen);
	writePos += writeLen;

	// Write startAt
	// TODO: Figure out how to specify time zone
	char timeStr[] = "2011-10-08T07:07:09";
	int timeStrLen = strlen(timeStr);
	struct tm *tmp = gmtime(&gameStartTime);
	_sprintf(
		&timeStr[0], "%04d-%02d-%02dT%02d:%02d:%02d", tmp->tm_year + 1900,
		tmp->tm_mon + 1, tmp->tm_mday, tmp->tm_hour, tmp->tm_min, tmp->tm_sec);
	u8 startAtOpener[] = {'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (u8)timeStrLen};
	writeLen = sizeof(startAtOpener);
	memcpy(&footer[writePos], startAtOpener, writeLen);
	writePos += writeLen;
	writeLen = timeStrLen;
	memcpy(&footer[writePos], timeStr, writeLen);
	writePos += writeLen;

	// Write lastFrame
	u8 lastFrameOpener[] = {'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l'};
	writeLen = sizeof(lastFrameOpener);
	memcpy(&footer[writePos], lastFrameOpener, writeLen);
	writePos += writeLen;
	memcpy(&footer[writePos], &lastFrame, 4);
	writePos += 4;

	// Write console nickname
	u8 nickLen = strlen(SlippiGetConsoleNick());
	if (nickLen > 32) nickLen = 32;
	u8 consoleNickOpener[] = { 'U', 11, 'c', 'o', 'n', 's', 'o', 'l', 'e', 'N', 'i', 'c', 'k', 'S', 'U', nickLen };
	writeLen = sizeof(consoleNickOpener);
	memcpy(&footer[writePos], consoleNickOpener, writeLen);
	writePos += writeLen;
	memcpy(&footer[writePos], SlippiGetConsoleNick(), nickLen);
	writePos += nickLen;

	// Write closing
	u8 closing[] = {
		'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{', '}',
		'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U',
		10, 'n', 'i', 'n', 't', 'e', 'n', 'd', 'o', 'n', 't',
		'}', '}'};
	writeLen = sizeof(closing);
	memcpy(&footer[writePos], closing, writeLen);
	writePos += writeLen;

	// Write footer
	// Always seek first in case there was a previous failure with partial write
	FRESULT fRes = f_lseek(file, writtenByteCount + 15);
	if (fRes != FR_OK)
	{
		dbgprintf("Slippi: failed to seek before writing footer, errno: %d\r\n", fRes);
		return fRes;
	}

	u32 wrote;
	fRes = f_write(file, footer, writePos, &wrote);
	if (fRes != FR_OK)
	{
		dbgprintf("Slippi: failed to write footer, errno: %d\r\n", fRes);
		return fRes;
	}

	// Write length
	fRes = f_lseek(file, 11);
	if (fRes != FR_OK)
	{
		dbgprintf("Slippi: failed to seek before writing length, errno: %d\r\n", fRes);
		return fRes;
	}

	fRes = f_write(file, &writtenByteCount, 4, &wrote);
	if (fRes != FR_OK)
		dbgprintf("Slippi: failed to write length, errno: %d\r\n", fRes);
	
	return fRes;
}

static u32 SlippiHandlerThread(void *arg)
{
	dbgprintf("Slippi Thread ID: %d\r\n", thread_get_id());

	static SlpGameReader reader;
	static u8 readBuf[READ_BUF_SIZE];
	static u64 memReadPos = 0;

	u32 writtenByteCount = 0;
	s32 lastFrame = 0;	// the open file's last frame so far, for completeFile
	driveTimer = read32(HW_TIMER);
	driveTimerSet = false;

	bool failedToMount = false;
	bool currentFileOpen = false;
	bool currentFileValid = false;
	bool currentFileSynced = false;	// LazyTO: the early f_sync is done for the open file
	const bool use_usb = ConfigGetUseUSB() != 1;
	bool mounted = use_usb ? USBStorage_IsInserted_SlippiThread() : true;

	while (1)
	{
		// Cycle time, look at const definition for more info
		mdelay(THREAD_CYCLE_TIME_MS);

		if (use_usb)
		{
			if (!USBStorage_IsInserted_SlippiThread())
			{
				if (mounted)
				{
					// unmount (cannot fail so no need to check return value)
					f_mount_char(NULL, "usb:", 1);
				}

				failedToMount = false;
				currentFileOpen = false;
				currentFileValid = false;
				mounted = false;
				continue;
			}
			else if (!mounted && !failedToMount)
			{
				FRESULT mountResult = f_mount_char(devices[1], "usb:", 1);
				if (mountResult != FR_OK)
				{
					dbgprintf("Slippi: failed to mount usb, errno: %d\r\n", mountResult);

					// only attempt to mount once, user can retry by re-inserting the device.
					failedToMount = true;
					continue;
				}

				// Create folder if it doesn't exist yet
				FRESULT mkdirResult = f_mkdir_secondary_drive("/Slippi");
				if (mkdirResult != FR_OK && mkdirResult != FR_EXIST)
				{
					dbgprintf("Slippi: failed to mkdir: /Slippi, errno: %d\r\n", mkdirResult);

					// only attempt to mount once, user can retry by re-inserting the device.
					failedToMount = true;
					continue;
				}

				// ignore anything already in the buffer. users should not expect to record a
				// game if the usb device is inserted after game start.
				memReadPos = SlippiRestoreReadPos();
				mounted = true;
			}
			if (!mounted)
				continue;
		}

		while (1)
		{
			// Read from memory and write to file
			SlpMemError err = SlippiMemoryRead(&reader, readBuf, READ_BUF_SIZE, memReadPos);
			// LazyTO: a match that never sent Game End (a soft reset,
			// training). readBuf holds the rest of it, up to the next match's
			// RECEIVE_COMMANDS at reader.lastReadPos (SlippiMemory.c:124-130,
			// 185), and goes through the same steps as any read below: when
			// the read began at a match's own start (isNewGame, a match cut
			// off within its first 4 KB), the dangling file is completed and
			// that match's record choice decides first, so the bytes land in
			// the file of the match they belong to, or are skipped. The
			// writer then resumes at the next match, whose new-game step
			// completes this file. Jumping to the write cursor, as other
			// errors do, would lose the next match too.
			bool cutShort = err == SLP_MEM_UNNEX_NG;
			if (err && !cutShort)
			{
				// all possible errors render the current file incompletable, so let's jump ahead
				currentFileValid = false;
				memReadPos = SlippiRestoreReadPos();
				if (currentFileOpen)
				{
					FRESULT closeResult = f_close(&currentFile);
					if (closeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to close incompletable file, errno: %d\r\n", closeResult);
					}
					else
					{
						currentFileOpen = false;
					}
				}
				break;
			}

			// (a cut-short match is over: the next one has started)
			if (!cutShort && reader.lastReadResult.bytesAvailable < READ_BUF_SIZE && !(currentFileValid && reader.lastReadResult.isGameEnd))
			{
				// LazyTO: the LED only while a file is being written, not for a
				// match the kiosk did not ask for
				if (replaysLED && currentFileValid)
					flashLED();
				break;
			}

			if (reader.lastReadResult.isNewGame)
			{
				if (currentFileValid)
				{
					FRESULT completeResult = completeFile(&currentFile, lastFrame, writtenByteCount);
					if (completeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to complete dangling file, errno: %d\r\n", completeResult);
						break;
					}
					currentFileValid = false;
				}
				if (currentFileOpen)
				{
					FRESULT closeResult = f_close(&currentFile);
					if (closeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to close dangling file, errno: %d\r\n", closeResult);
						break;
					}
					currentFileOpen = false;
				}

				// LazyTO record gate: the choice made at this match's Game Start
				// (RelayEXI.c RelayEXIGateStart). The read starts at its
				// RECEIVE_COMMANDS (SlippiMemory.c:132-138), the cursor the
				// choice was kept under. Not recorded: no file at all, and the
				// skip path below drains the match.
				u32 gateSeq;
				bool record = RelayEXIGateChoice((u32)memReadPos, &gateSeq);
				if (gateSeq == 0)
					dbgprintf("Slippi: no record choice for the match at 0x%08x, recording it\r\n", (u32)memReadPos);
				if (!record)
					dbgprintf("Slippi: game start %u not recorded\r\n", gateSeq);
				else
				{
					dbgprintf("Creating File...\r\n");
					gameStartTime = GetCurrentTime();
					char *fileName = generateFileName();
					// Maybe can remove FA_READ since network thread doesn't share &currentFile
					FRESULT fileOpenResult = f_open_secondary_drive(&currentFile, fileName, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
					if (fileOpenResult != FR_OK)
					{
						dbgprintf("Slippi: failed to open file: %s, errno: %d\r\n", fileName, fileOpenResult);
						break;
					}

					currentFileOpen = true;
					currentFileSynced = false;
					writtenByteCount = 0;
					lastFrame = 0;

					FRESULT writeHeaderResult = writeHeader(&currentFile);
					if (writeHeaderResult != FR_OK)
					{
						dbgprintf("Slippi: failed to write header, errno: %d\r\n", writeHeaderResult);
						break;
					}

					currentFileValid = true;
					// tell the kiosk which Game Start this file is, and its name
					if (gateSeq != 0)
						RelayEXIGateOpened(gateSeq, gameStartTime);
				}
			}

			if (!currentFileValid)
			{
				// we can reach this state if we SlippiRestoreReadPos into
				// the middle of a game due to usb insertion or SlpMemError,
				// or for a match the kiosk did not ask for (LazyTO)
				// skip over and don't write anything until we see the start of a new game
				// LazyTO: everything available now, not one chunk per cycle (a
				// long skipped match could outrun 4 KB per 100 ms, overflow the
				// ring and lose the next match's start), and without the LED
				memReadPos += reader.lastReadResult.bytesRead;
				if (cutShort)
					dbgprintf("Slippi: match ended without Game End, on to the next one\r\n");
				if (reader.lastReadResult.bytesRead == 0)
					break;
				continue;
			}

			// Always seek first in case there was a previous failure with partial write
			FRESULT seekResult = f_lseek(&currentFile, writtenByteCount + 15);
			if (seekResult != FR_OK)
			{
				dbgprintf("Slippi: failed to seek before writing data, errno: %d\r\n", seekResult);
				break;
			}

			UINT wrote;
			FRESULT writeResult = f_write(&currentFile, readBuf, reader.lastReadResult.bytesRead, &wrote);
			if (writeResult != FR_OK)
			{
				dbgprintf("Slippi: failed to write data, errno: %d\r\n", writeResult);
				break;
			}
			else if (cutShort && wrote != reader.lastReadResult.bytesRead)
			{
				// all or nothing, so the next read still starts at a
				// command: tried again next cycle, from the same place
				dbgprintf("Slippi: wrote %u of %u bytes of a match without Game End\r\n", wrote, reader.lastReadResult.bytesRead);
				break;
			}
			else
			{
				// Only increment mem read position when the write fully succeeds
				memReadPos += wrote;
				writtenByteCount += wrote;
				lastFrame = reader.metadata.lastFrame;

				// LazyTO: one f_sync after the first data block of each
				// recording (Event Payloads and Game Start), some 0.2-0.3 s in
				// (estimated). The directory entry then owns the cluster chain,
				// so a recording cut off later (Wii off, exit to the loader,
				// beamer unplugged) is a file with a size, not a 0-byte entry
				// with leaked clusters, and the beamer can see the game as
				// live. Upstream #66 dropped the per-write syncs for flash
				// wear; this is one per file. A failed sync leaves the file
				// usable, so it is only logged.
				if (!currentFileSynced && !reader.lastReadResult.isGameEnd)
				{
					FRESULT syncResult = f_sync(&currentFile);
					if (syncResult != FR_OK)
						dbgprintf("Slippi: failed to sync new file, errno: %d\r\n", syncResult);
					currentFileSynced = true;
				}

				if (reader.lastReadResult.isGameEnd)
				{
					dbgprintf("Completing File...\r\n");
					FRESULT completeResult = completeFile(&currentFile, lastFrame, writtenByteCount);
					if (completeResult != FR_OK)
					{
						// error is logged in completeFile
						break;
					}

					currentFileValid = false;
					FRESULT closeResult = f_close(&currentFile);
					if (closeResult != FR_OK)
					{
						dbgprintf("Slippi: failed to close completed file, errno: %d\r\n", closeResult);
					}
					else
					{
						currentFileOpen = false;
						if (replaysLED)
							flashLED();
					}

					break;
				}
				else if (replaysLED)
					flashLED();
				if (cutShort)
					dbgprintf("Slippi: match ended without Game End, on to the next one\r\n");
			}
		}
	}

	return 0;
}
