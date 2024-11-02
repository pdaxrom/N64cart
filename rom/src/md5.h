// MD5 (Message-Digest Algorithm 5)
// Copyright Victor Breder 2024
// SPDX-License-Identifier: MIT

#pragma once
#include <stdint.h>

typedef struct {
    uint32_t a, b, c, d;
} md5_context;

void md5_init(md5_context* ctx);
void md5_digest(md5_context* ctx, void* buffer, size_t size);
void md5_output(md5_context* ctx, uint8_t out[16]);
