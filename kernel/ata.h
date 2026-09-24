#ifndef FOS_ATA_H
#define FOS_ATA_H

#include "types.h"

#define ATA_SECTOR_SIZE 512

int  ata_init(void);
int  ata_available(void);
int  ata_read_sector(u32 lba, void *buf);
int  ata_write_sector(u32 lba, const void *buf);
u32  ata_sector_count(void);

#endif