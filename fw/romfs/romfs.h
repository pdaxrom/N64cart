#ifndef __ROMFS_H__
#define __ROMFS_H__

#define ROMFS_MAX_NAME_LEN 56

#define ROMFS_MODE_READWRITE	(0)
#define ROMFS_MODE_READONLY	(1 << 0)
#define ROMFS_MODE_SYSTEM	(1 << 1)
#define ROMFS_MODE_RESERVED	(1 << 2)

#define ROMFS_TYPE_FIRMWARE	(0x00)
#define ROMFS_TYPE_FLASHLIST	(0x01)
#define ROMFS_TYPE_FLASHMAP	(0x02)
#define ROMFS_TYPE_MISC		(0xff)

typedef struct __attribute__((packed)){
    char     name[ROMFS_MAX_NAME_LEN];
    uint16_t mode: 3;
    uint16_t type: 13;
    uint16_t start;
    uint32_t size;
} romfs_entry;

typedef struct {
    romfs_entry entry;
    uint16_t nentry;
    uint16_t pos;
    uint16_t offset;
} romfs_file;

bool romfs_start(uint8_t *mem, uint32_t start, uint32_t size);
bool romfs_format();
uint32_t romfs_free();
bool romfs_create_file(char *name, romfs_file *file);
bool romfs_write_file(void *buffer, uint32_t size, romfs_file *file);
bool romfs_open_file(char *name, romfs_file *file);
bool romfs_read_file(void *buffer, uint32_t size, romfs_file *file);
bool romfs_list(romfs_file *entry, bool first);
bool romfs_delete(const char *name);


void save_romfs(char *name, uint8_t *mem, size_t len);
bool read_romfs(char *name, uint8_t *mem, size_t len);

#endif
