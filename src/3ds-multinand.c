#include <windows.h>
#include <winioctl.h>
#include <commctrl.h>
#include "3ds-multinand.h"

//#define DEBUG_BUILD

/* To do: add compatibility with strings from more flashcards */
const uint8_t MAGIC_STR[3][11] = {	{ 0x47, 0x41, 0x54, 0x45, 0x57, 0x41, 0x59, 0x4E, 0x41, 0x4E, 0x44 }, // "GATEWAYNAND"
									{ 0x4D, 0x54, 0x43, 0x41, 0x52, 0x44, 0x5F, 0x4E, 0x41, 0x4E, 0x44 }, // "MTCARD_NAND"
									{ 0x33, 0x44, 0x53, 0x43, 0x41, 0x52, 0x44, 0x4E, 0x41, 0x4E, 0x44 }  // "3DSCARDNAND"
								 };

static wchar_t wc[512] = {0};
static wchar_t msg_info[512] = {0};

int GetTextSize(LPTSTR str)
{
	int i = 0;
	while (str[i] != '\0') i++;
	return i;
}

wchar_t *GetWC(const char *c, uint8_t len)
{
	if (len > 512) return NULL;
	mbstowcs(wc, c, len);
	return wc;
}

int64_t set_file_pointer(HANDLE h, int64_t new_ptr, uint32_t method)
{
	int32_t hi_ptr = PTR_HIGH(new_ptr);
	int32_t lo_ptr = PTR_LOW(new_ptr);

	lo_ptr = SetFilePointer(h, lo_ptr, (PLONG)&hi_ptr, method);
	if (lo_ptr != -1 && hi_ptr != -1)
	{
/*#ifdef DEBUG_BUILD
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"cur_ptr: 0x%08x%08x.", hi_ptr, lo_ptr);
		MessageBox(NULL, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
		return PTR_FULL(hi_ptr, lo_ptr);
	}
	
	return -1;
}

int64_t get_drive_size(HANDLE h)
{
	uint32_t status, returned;
	DISK_GEOMETRY_EX DiskGeometry;
	
    status = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &DiskGeometry, sizeof(DISK_GEOMETRY_EX), (PDWORD)&returned, NULL);
    if (!status) return -1;
	
	if (DiskGeometry.DiskSize.QuadPart > 0) return DiskGeometry.DiskSize.QuadPart;
	
	return 0;
}

bool write_dummy_data(HANDLE SDcard, int64_t offset)
{
	int64_t ptr = set_file_pointer(SDcard, offset, FILE_BEGIN);
	if (ptr != -1)
	{
		uint32_t bytes = 0;
		uint8_t dummy_buf[SECTOR_SIZE] = {0};
		
		/* Fill buffer with dummy data */
		for (int i = 0; i < SECTOR_SIZE; i += 2)
		{
			dummy_buf[i] = ((DUMMY_DATA >> 8) & 0xff);
			dummy_buf[i+1] = (DUMMY_DATA & 0xff);
		}
		
		/* Write dummy data */
		int res = WriteFile(SDcard, dummy_buf, SECTOR_SIZE, (PDWORD)&bytes, NULL);
		if (res && bytes == SECTOR_SIZE)
		{
/*#ifdef DEBUG_BUILD
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Wrote %d bytes long \"0x%04X\" dummy data at offset 0x%09llX.", SECTOR_SIZE, DUMMY_DATA, offset);
			MessageBox(NULL, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
			return true;
		} else {
			MessageBox(NULL, TEXT("Error writing dummy data."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		}
	} else {
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
		MessageBox(NULL, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
	}
	
	return false;
}

void MultiNandProc(wchar_t *fname, HWND hWndParent, HWND hWndProgress)
{
#ifdef DEBUG_BUILD
	uint32_t drivenum;
#endif
	
	int i, dev_res;
	int64_t cur_ptr = -1;
	FILE *nandfile = NULL;
	wchar_t devname[30] = {0};
	uint8_t buf[SECTOR_SIZE] = {0};
	HANDLE drive = INVALID_HANDLE_VALUE;
	
	uint32_t DriveLayoutLen = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + (3 * sizeof(PARTITION_INFORMATION_EX));
	DRIVE_LAYOUT_INFORMATION_EX *DriveLayout = malloc(DriveLayoutLen);
	
	uint8_t mbr_str = 0;
	int64_t fatsector = (!n3ds ? ((int64_t)O3DS_FS_BASE_SECTOR * nandnum) : ((int64_t)N3DS_FS_BASE_SECTOR * nandnum));
	int64_t nandsect = (!n3ds ? (SECTOR_SIZE + ((int64_t)O3DS_FS_BASE_SECTOR * (nandnum - 1))) : (SECTOR_SIZE + ((int64_t)N3DS_FS_BASE_SECTOR * (nandnum - 1))));
	
	/* Get the total amount of available logical drives */
	wchar_t *SingleDrive;
	wchar_t LogicalDrives[MAX_PATH] = {0};
	uint32_t res = GetLogicalDriveStrings(MAX_PATH, LogicalDrives);
	if (res > 0 && res <= MAX_PATH)
	{
		/* Try to open each logical drive through CreateFile(), in order to get their physical drive number */
		/* This is because we won't operate on a filesystem level */
		/* Once we get the physical drive number, we'll close the current handle and try to open the disk as a physical media */
		SingleDrive = LogicalDrives;
		while (*SingleDrive)
		{
			/* Skip drives A:, B: and C: to avoid problems */
			if (SingleDrive[0] != 'A' && SingleDrive[0] != 'B' && SingleDrive[0] != 'C')
			{
				/* Get the drive type */
				res = GetDriveType(SingleDrive);
				if (res == DRIVE_REMOVABLE || res == DRIVE_FIXED)
				{
					/* Open logical drive */
					_snwprintf(devname, MAX_CHARACTERS(devname), L"\\\\.\\%c:", SingleDrive[0]);
					drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
					if (drive != INVALID_HANDLE_VALUE)
					{
						/* Check if the drive is ready */
						dev_res = DeviceIoControl(drive, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, (PDWORD)&res, NULL);
						if (dev_res != 0)
						{
							VOLUME_DISK_EXTENTS diskExtents;
							dev_res = DeviceIoControl(drive, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, (void*)&diskExtents, (uint32_t)sizeof(diskExtents), (PDWORD)&res, NULL);
							
							CloseHandle(drive);
							drive = INVALID_HANDLE_VALUE;
							
							if (dev_res && res > 0)
							{
								/* Then again, we don't want to run the code on drive #0 */
								/* This may be an additional partition placed after C: */
								if (diskExtents.Extents[0].DiskNumber > 0)
								{
									/* Open physical drive */
									_snwprintf(devname, MAX_CHARACTERS(devname), L"\\\\.\\PhysicalDrive%u", (unsigned int)diskExtents.Extents[0].DiskNumber);
									drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
									if (drive != INVALID_HANDLE_VALUE)
									{
										/* Check if this is actually the SD card that contains the EmuNAND */
										/* Just for safety, let's set the file pointer to zero before attempting to read */
										cur_ptr = set_file_pointer(drive, 0, FILE_BEGIN);
										if (cur_ptr != -1)
										{
											/* Read operations have to be aligned to 512 bytes in order to get this to work */
											dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
											if (dev_res && res == SECTOR_SIZE)
											{
												for (i = 0; i < 3; i++)
												{
													if (memcmp(buf, &(MAGIC_STR[i][0]), 11) == 0)
													{
														/* Found it! */
														mbr_str = (i + 1);
#ifdef DEBUG_BUILD
														drivenum = diskExtents.Extents[0].DiskNumber;
#endif
														break;
													}
												}
												
												if (mbr_str > 0)
												{
													break;
												} else {
#ifdef DEBUG_BUILD
													_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"MBR signature (logical drive \"%c:\"):\n", SingleDrive[0]);
													for (i = 0; i < 11; i++)
													{
														wchar_t byte[4] = {0};
														_snwprintf(byte, 3, L"%02X ", buf[i]);
														wcscat(msg_info, byte);
													}
													MessageBox(hWndParent, msg_info, TEXT("Debug info"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
												}
											} else {
#ifdef DEBUG_BUILD
												_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't read %d bytes chunk from \"%s\" sector #0 (%d).", SECTOR_SIZE, devname, GetLastError());
												MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
											}
										} else {
#ifdef DEBUG_BUILD
											_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't set the file pointer to zero in \"%s\".", devname);
											MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
										}
										
										CloseHandle(drive);
										drive = INVALID_HANDLE_VALUE;
									} else {
#ifdef DEBUG_BUILD
										_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open physical drive \"%s\" (%d).", devname, GetLastError());
										MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
									}
								}
							} else {
#ifdef DEBUG_BUILD
								_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't retrieve disk volume information for \"%s\".\ndev_res: %d / res: %u.", devname, dev_res, res);
								MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
							}
						} else {
#ifdef DEBUG_BUILD
							_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Logical drive \"%c:\" not ready (empty drive?).", SingleDrive[0]);
							MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
							CloseHandle(drive);
							drive = INVALID_HANDLE_VALUE;
						}
					} else {
#ifdef DEBUG_BUILD
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open logical drive \"%s\" (%d).", devname, GetLastError());
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
					}
				} else {
					if (res == DRIVE_UNKNOWN)
					{
#ifdef DEBUG_BUILD
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Unknown drive type for \"%c:\".", SingleDrive[0]);
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
					}
				}
			}
			
			/* Get the next drive */
			SingleDrive += GetTextSize(SingleDrive) + 1;
		}
		
		if (drive == INVALID_HANDLE_VALUE)
		{
			MessageBox(hWndParent, TEXT("Unable to identify the SD card that contains the EmuNAND."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	} else {
		MessageBox(hWndParent, TEXT("Couldn't parse logical drive strings."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
#ifdef DEBUG_BUILD
	_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Found %s SD card!\nLogical drive: %s.\nPhysical drive number: %d.", GetWC((char*)(&(MAGIC_STR[mbr_str - 1][0])), 11), SingleDrive, drivenum);
	MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
	
	/* Get disk geometry */
	int64_t drive_sz = get_drive_size(drive);
	if (drive_sz == -1)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't get the disk geometry (%d).", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	} else
	if (drive_sz == 0)
	{
		MessageBox(hWndParent, TEXT("Drive size is zero!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	} else {
#ifdef DEBUG_BUILD
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Drive capacity: %I64d bytes.", drive_sz);
		MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
		if ((!n3ds && ((nandnum == 1 && ((drive_sz / 1000000) <= 1024)) || (nandnum > 1 && ((drive_sz / 1000000) <= 2048)))) || (n3ds && ((nandnum == 1 && ((drive_sz / 1000000) <= 2048)) || (nandnum > 1 && ((drive_sz / 1000000) <= 4096)))))
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Your SD card must have a capacity of at least %c GB.", (nandnum == 1 ? (!n3ds ? L'2' : L'4') : (!n3ds ? L'4' : L'8')));
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	}
	
	/* Check if the SD card is write-protected */
	if (is_input)
	{
		uint32_t sys_flags = 0;
		dev_res = GetVolumeInformation(SingleDrive, NULL, 0, NULL, NULL, (PDWORD)&sys_flags, NULL, 0);
		if (dev_res != 0)
		{
			if (sys_flags & FILE_READ_ONLY_VOLUME)
			{
				MessageBox(hWndParent, TEXT("The SD card is write-protected!\nMake sure the lock slider is not in the \"locked\" position."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				goto out;
			}
		} else {
#ifdef DEBUG_BUILD
			MessageBox(hWndParent, TEXT("Couldn't get the file system flags.\nNot really a critical problem. Process won't be halted."), TEXT("Error"), MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
#endif
		}
	}
	
	/* Get drive layout */
	dev_res = DeviceIoControl(drive, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, DriveLayout, DriveLayoutLen, (PDWORD)&res, NULL);
	if (dev_res)
	{
		if (DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT32_LBA || DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT32 || DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT16)
		{
			/* Only use FAT16 if the SD card capacity is <= 4 GB or if the partition was already FAT16 */
			bool is_fat16 = (DriveLayout->PartitionEntry[0].Mbr.PartitionType == PARTITION_FAT16 || ((drive_sz / 1000000) <= 4096));
			
			if (nandnum > 1)
			{
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					if (is_input)
					{
						/* Move filesystem to the right */
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition detected below the required offset (at 0x%09llX).\nDo you want to begin the drive layout modification procedure?\n\nPlease, bear in mind that doing so will wipe the data on your current FAT partition.\nIf you select \"No\", the process will be canceled.", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart);
						dev_res = MessageBox(hWndParent, msg_info, TEXT("Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
						if (dev_res == IDNO)
						{
							goto out;
						} else {
							memset(DriveLayout, 0, DriveLayoutLen);
							
							DriveLayout->PartitionStyle = PARTITION_STYLE_MBR;
							DriveLayout->PartitionCount = 4; // Minimum required by MBR
							DriveLayout->Mbr.Signature = FAT32_SIGNATURE;
							
							DriveLayout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_MBR;
							DriveLayout->PartitionEntry[0].StartingOffset.QuadPart = fatsector;
							DriveLayout->PartitionEntry[0].PartitionLength.QuadPart = (drive_sz - fatsector);
							DriveLayout->PartitionEntry[0].PartitionNumber = 1;
							
							DriveLayout->PartitionEntry[0].Mbr.PartitionType = (is_fat16 ? PARTITION_FAT16 : PARTITION_FAT32_LBA);
							DriveLayout->PartitionEntry[0].Mbr.BootIndicator = FALSE;
							DriveLayout->PartitionEntry[0].Mbr.RecognizedPartition = 1;
							DriveLayout->PartitionEntry[0].Mbr.HiddenSectors = 0;
							
							for (i = 0; i < 4; i++) DriveLayout->PartitionEntry[i].RewritePartition = TRUE;
							
							dev_res = DeviceIoControl(drive, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, DriveLayout, DriveLayoutLen, NULL, 0, (PDWORD)&res, NULL);
							if (dev_res)
							{
#ifdef DEBUG_BUILD
								_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition successfully moved to offset 0x%09llX!", fatsector);
								MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
								char format_cmd[64] = {0};
								snprintf(format_cmd, MAX_CHARACTERS(format_cmd), "FORMAT %c: /Y /FS:%s /V:%.11s /Q /X", SingleDrive[0], (is_fat16 ? "FAT" : "FAT32"), (char*)(&(MAGIC_STR[mbr_str - 1][0])));
								
								CloseHandle(drive);
								
								/* Format the new partition */
								dev_res = system(format_cmd);
								if (dev_res == 0)
								{
									/* Reopen the handle to the physical drive */
									drive = CreateFile(devname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
									if (drive != INVALID_HANDLE_VALUE)
									{
										MessageBox(hWndParent, TEXT("Successfully formatted the new FAT partition!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
										
										/* Set file pointer to (nandsect - SECTOR_SIZE) and write the dummy header */
										if (!write_dummy_data(drive, nandsect - SECTOR_SIZE)) goto out;
									} else {
										MessageBox(hWndParent, TEXT("Couldn't reopen the handle to the physical drive!"), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
										goto out;
									}
								} else {
									_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't format the new FAT partition! (%d).", dev_res);
									MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
									goto out;
								}
							} else {
								MessageBox(hWndParent, TEXT("Couldn't modify the drive layout."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
								goto out; 
							}
						}
					} else {
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition offset (0x%09llX) collides with the **%s** EmuNAND offset (0x%09llX). The %s %c GiB segment after 0x%09llX hasn't been created!", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart, NAND_NUM_STR(nandnum), nandsect, NAND_NUM_STR(nandnum), (!n3ds ? '1' : '2'), nandsect - SECTOR_SIZE - 1);
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						goto out;
					}
				} else {
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition already positioned beyond offset 0x%09llX.", fatsector - 1);
					if (is_input) wcscat(msg_info, L"\nSkipping drive layout modification procedure.");
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
				}
			} else {
				if (DriveLayout->PartitionEntry[0].StartingOffset.QuadPart < fatsector)
				{
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition offset (0x%09llX) collides with the **%s** EmuNAND offset (0x200). This is probably some kind of corruption. Format the EmuNAND again on your Nintendo 3DS console to fix this.", DriveLayout->PartitionEntry[0].StartingOffset.QuadPart, NAND_NUM_STR(nandnum));
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
					goto out;
				} else {
#ifdef DEBUG_BUILD
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"FAT partition already positioned beyond offset 0x%09llX.", fatsector - 1);
					MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
				}
			}
		} else {
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Invalid partition type: %02X.\nYou should report this to me ASAP.", DriveLayout->PartitionEntry[0].Mbr.PartitionType);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	} else {
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't get the drive layout (%d).", GetLastError());
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	int64_t offset = 0;
	uint32_t nandsize = 0, magic_word = 0;
	
	/* Open 3DS NAND dump */
	nandfile = _wfopen(fname, (is_input ? L"rb" : L"wb"));
	if (!nandfile)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't open \"%s\" for %s.", fname, (is_input ? L"reading" : L"writing"));
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	if (is_input)
	{
		/* Store NAND dump size */
		fseek(nandfile, 0, SEEK_END);
		nandsize = ftell(nandfile);
		rewind(nandfile);
		
		switch (nandsize)
		{
			case TOSHIBA_NAND:
			case TOSHIBA_REDNAND:
			case SAMSUNG_NAND:
			case SAMSUNG_REDNAND:
				if (n3ds)
				{
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Invalid 3DS NAND dump.\nFilesize (%u bytes) is invalid.", nandsize);
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
					goto out;
				}
				break;
			case N3DS_SAMSUNG_NAND:
			case N3DS_UNKNOWN_NAND:
				if (!n3ds)
				{
					_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Invalid 3DS NAND dump.\nFilesize (%u bytes) is invalid.", nandsize);
					MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
					goto out;
				}
				break;
			default:
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Invalid 3DS NAND dump.\nFilesize (%u bytes) is invalid.", nandsize);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				goto out;
		}

		bool is_rednand = (nandsize == TOSHIBA_REDNAND || nandsize == SAMSUNG_REDNAND);
		if (is_rednand && !cfw) cfw = true; // Override configuration
		
#ifdef DEBUG_BUILD
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"%s 3DS %s NAND dump detected!\nFilesize: %u bytes.", (!n3ds ? L"Old" : L"New"), NAND_TYPE_STR(nandsize), nandsize);
		MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
		/* Check if the supplied NAND dump does contain an NCSD header */
		fseek(nandfile, (is_rednand ? (SECTOR_SIZE + 0x100) : 0x100), SEEK_SET);
		fread(&magic_word, 4, 1, nandfile);
		rewind(nandfile);
		
		if (magic_word == bswap_32(NCSD_MAGIC))
		{
#ifdef DEBUG_BUILD
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Valid NCSD header detected at offset 0x%08x.", (is_rednand ? SECTOR_SIZE : 0));
			MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
			if (is_rednand)
			{
				/* Skip the dummy header (if it's already present) */
				fseek(nandfile, SECTOR_SIZE, SEEK_SET);
				nandsize -= SECTOR_SIZE;
			}
		} else {
			MessageBox(hWndParent, TEXT("Invalid 3DS NAND dump.\nThe NCSD header is missing."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		}
	} else {
		/* Check if the NAND dump stored in the SD card is valid */
		/* Depending on the type of the NAND stored in the SD card, the data has to be read in different ways */
		/* This is because the order in which the data is written varies between each type of NAND */
		
		/* EmuNAND: the first 0x200 bytes (NCSD header) are stored **after** the NAND dump. The NAND dump starts to be written */
		/*			from offset 0x200 to the SD card */
		
		/* RedNAND: used by the Custom Firmware (CFW). It is written to the SD card 'as is', beginning with the NCSD header and */
		/*			following with the rest of the NAND data */
		
		/* Usually, a dummy footer follows afterwards. This applies for both types of NANDs, and serves to indicate the appropiate */
		/* NAND flash capacity of the 3DS console */
		
		/* The CFW is so far the only loading method that supports using a custom boot sector (different to 1) */
		/* This is expected to change in the near future */
		
		for (i = 0; i <= 2; i++)
		{
			if (n3ds && i == 2) break;
			
			/* Old 3DS: Check if this is a RedNAND (i = 0), Toshiba EmuNAND (i = 1) or Samsung EmuNAND (i = 2), in that order */
			/* New 3DS: Check if this is a N3DS Samsung EmuNAND (i = 0) or a N3DS **Unknown** EmuNAND (i = 1), in that order */
			
			switch (i)
			{
				case 0:
					if (!n3ds)
					{
						/* Old 3DS RedNAND */
						offset = nandsect;
					} else {
						/* New 3DS Samsung EmuNAND */
						offset = (nandsect + N3DS_SAMSUNG_NAND - SECTOR_SIZE);
					}
					break;
				case 1:
					if (!n3ds)
					{
						/* Old 3DS Toshiba EmuNAND */
						offset = (nandsect + TOSHIBA_NAND - SECTOR_SIZE);
					} else {
						/* New 3DS **Unknown** EmuNAND */
						offset = (nandsect + N3DS_UNKNOWN_NAND - SECTOR_SIZE);
					}
					break;
				case 2:
					/* Old 3DS Samsung EmuNAND */
					offset = (nandsect + SAMSUNG_NAND - SECTOR_SIZE);
					break;
				default:
					break;
			}
			
			cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
			if (cur_ptr != -1)
			{
				/* Remember that read/write operations must be aligned to 512 bytes */
				dev_res = ReadFile(drive, buf, SECTOR_SIZE, (PDWORD)&res, NULL);
				if (dev_res && res == SECTOR_SIZE)
				{
					memcpy(&magic_word, &(buf[0x100]), 4);
					if (magic_word == bswap_32(NCSD_MAGIC))
					{
#ifdef DEBUG_BUILD
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Valid NCSD header detected at offset 0x%09llX (%s).", offset, ((i == 0 && !n3ds) ? L"RedNAND" : L"EmuNAND"));
						MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
						/* No need to calculate what we already know */
						switch (i)
						{
							case 0:
								if (n3ds) nandsize = N3DS_SAMSUNG_NAND;
								break;
							case 1:
								nandsize = (!n3ds ? TOSHIBA_NAND : N3DS_UNKNOWN_NAND);
								break;
							case 2:
								nandsize = SAMSUNG_NAND;
								break;
							default:
								break;
						}
						
						/* RedNAND size calculation procedure */
						if (nandsize == 0)
						{
							/* Override configuration */
							cfw = true;
							
							/* Calculate NAND dump size (based in the dummy footer position) */
							/* This steps will only be performed on a RedNAND */
							int j = 0;
							uint16_t data = 0;
							uint8_t dummy[SECTOR_SIZE] = {0};
							
							while (j < 2)
							{
								/* Check if this is a Toshiba RedNAND (j = 0) or Samsung RedNAND (j = 1), in that order */
								offset = (j == 0 ? (nandsect + TOSHIBA_NAND) : (nandsect + SAMSUNG_NAND));
								cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
								if (cur_ptr == -1) break;
								
								/* Can't lose that copy of the NCSD header stored in buf. We'll be needing it at a later time */
								dev_res = ReadFile(drive, dummy, SECTOR_SIZE, (PDWORD)&res, NULL);
								if (!dev_res || res != SECTOR_SIZE) break;
								
								/* Check if this block contains the dummy footer */
								int k;
								for (k = 0; k < SECTOR_SIZE; k += 2)
								{
									memcpy(&data, &(dummy[k]), 2);
									if (data != bswap_16(DUMMY_DATA)) break;
								}
								
								if (data == bswap_16(DUMMY_DATA))
								{
									/* Found it */
									nandsize = (j == 0 ? TOSHIBA_NAND : SAMSUNG_NAND);
									break;
								}
								
								j++;
							}
							
							if (cur_ptr == -1 || !dev_res || res != SECTOR_SIZE || nandsize > 0) break;
							
							MessageBox(hWndParent, TEXT("Dummy footer not available in the SD card.\nRedNAND size will be calculated based in the NCSD header info.\nIt may not match your actual NAND flash capacity."), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
							
							/* Calculate NAND dump size (based in the NCSD header info) */
							/* May not match actual NAND flash capacity */
							for (j = 0x124; j < 0x160; j += 8)
							{
								uint32_t partition_len = 0;
								memcpy(&partition_len, &(buf[j]), 4);
								nandsize += (partition_len * MEDIA_UNIT_SIZE);
							}
						}
						
						break;
					}
				} else {
					break;
				}
			} else {
				break;
			}
		}
		
		if (cur_ptr == -1)
		{
			_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
			MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
			goto out;
		} else {
			if (!dev_res || res != SECTOR_SIZE)
			{
				_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't read %d bytes block from offset 0x%09llX.", SECTOR_SIZE, offset);
				MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
				goto out;
			} else {
				if (magic_word != bswap_32(NCSD_MAGIC))
				{
					MessageBox(hWndParent, TEXT("Invalid 3DS NAND dump.\nThe NCSD header is missing."), TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
					goto out;
				} else {
					if (nandsize == TOSHIBA_NAND || nandsize == SAMSUNG_NAND || nandsize == N3DS_SAMSUNG_NAND || nandsize == N3DS_UNKNOWN_NAND)
					{
#ifdef DEBUG_BUILD
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"%s 3DS %s NAND dump detected!\nFilesize: %u bytes.", (!n3ds ? L"Old" : L"New"), NAND_TYPE_STR(nandsize), nandsize);
						MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif
					} else {
						_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Invalid 3DS NAND dump.\nFilesize (%u bytes) is invalid.", nandsize);
						MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
						goto out;
					}
				}
			}
		}
	}
	
/*#ifdef DEBUG_BUILD
	_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"%s %s %sNAND %s the SD card, please wait...", (is_input ? L"Writing" : L"Reading"), NAND_NUM_STR(nandnum), (cfw ? L"Red" : L"Emu"), (is_input ? L"to" : L"from"));
	MessageBox(hWndParent, msg_info, TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
#endif*/
	
	uint32_t cnt;
	uint8_t *nand_buf = malloc(NAND_BUF_SIZE);
	
	/* Set the progress bar range */
	SendMessage(hWndProgress, PBM_SETRANGE, 0, MAKELPARAM(0, (nandsize / NAND_BUF_SIZE)));
	
	/* The real magic begins here */
	for (cnt = 0; cnt < nandsize; cnt += NAND_BUF_SIZE)
	{
		/* Set file pointer before doing any read/write operation */
		/* Remember to appropiately set the file pointer to the end of the NAND dump when dealing with the NCSD header (EmuNAND only) */
		offset = (cfw ? (nandsect + cnt) : (cnt > 0 ? (nandsect - SECTOR_SIZE + cnt) : (is_input ? (nandsect - SECTOR_SIZE + nandsize) : (nandsect - SECTOR_SIZE))));
		cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
		if (cur_ptr == -1) break;
		
		if (is_input)
		{
			/* Fill buffer (file) */
			fread(nand_buf, NAND_BUF_SIZE, 1, nandfile);
			
			if (!cfw && cnt == 0)
			{
				/* Write the NCSD header contained in nand_buf */
				dev_res = WriteFile(drive, nand_buf, SECTOR_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != SECTOR_SIZE) break;
				
				/* Go back to sector #1 */
				offset = nandsect;
				cur_ptr = set_file_pointer(drive, offset, FILE_BEGIN);
				if (cur_ptr == -1) break;
				
				/* Write the rest of the buffer */
				dev_res = WriteFile(drive, &(nand_buf[SECTOR_SIZE]), NAND_BUF_SIZE - SECTOR_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != (NAND_BUF_SIZE - SECTOR_SIZE)) break;
			} else {
				/* Write buffer (SD) */
				dev_res = WriteFile(drive, nand_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
				if (!dev_res || res != NAND_BUF_SIZE) break;
			}
			
			/* Check if this is the last chunk */
			if (nandsize == cnt + NAND_BUF_SIZE)
			{
				/* Write the dummy footer */
				if (!write_dummy_data(drive, (cfw ? (offset + NAND_BUF_SIZE) : (offset + NAND_BUF_SIZE + SECTOR_SIZE)))) break;
			}
		} else {
			/* Fill buffer (SD) */
			dev_res = ReadFile(drive, nand_buf, NAND_BUF_SIZE, (PDWORD)&res, NULL);
			if (!dev_res || res != NAND_BUF_SIZE) break;
			
			/* Replace the first 512-bytes block in nand_buf with the NCSD header (still contained in buf) */
			if (!cfw && cnt == 0) memcpy(nand_buf, buf, SECTOR_SIZE);
			
			/* Write buffer (file) */
			fwrite(nand_buf, 1, NAND_BUF_SIZE, nandfile);
		}
		
		/* Update the progress bar */
		SendMessage(hWndProgress, PBM_STEPIT, 0, 0);
	}
	
	free(nand_buf);
	
	if (cur_ptr == -1)
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't seek to offset 0x%09llX in physical drive.", offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	} else
	if (!dev_res || (cnt == 0 && !cfw && res != (NAND_BUF_SIZE - SECTOR_SIZE)) || (cnt > 0 && res != NAND_BUF_SIZE))
	{
		_snwprintf(msg_info, MAX_CHARACTERS(msg_info), L"Couldn't %s block #%u %s offset 0x%09llX.", (is_input ? L"write" : L"read"), cnt / NAND_BUF_SIZE, (is_input ? L"to" : L"from"), offset);
		MessageBox(hWndParent, msg_info, TEXT("Error"), MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
		goto out;
	}
	
	MessageBox(hWndParent, TEXT("Operation successfully completed!"), TEXT("Information"), MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
	
out:
	if (DriveLayout) free(DriveLayout);
	if (nandfile) fclose(nandfile);
	if (drive != INVALID_HANDLE_VALUE) CloseHandle(drive);
}
