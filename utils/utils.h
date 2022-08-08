#ifndef __UTILS_H__
#define __UTILS_H__

#define DATA_UNKNOWN	0xdeadbeaf
#define DATA_RETRY	0x43212343
#define DATA_REPLY	0x43212344
#define DATA_WRITE	0x43212345
#define DATA_READ	0x43212346

struct __attribute__((__packed__)) data_header {
    uint32_t	type;
    uint32_t	address;
    uint32_t	length;
};

#endif