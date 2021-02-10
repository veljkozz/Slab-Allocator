#pragma once

#define BLOCK_SIZE (4096)

#define TRUE 1
#define FALSE 0

typedef unsigned long UL;
typedef unsigned long long ULL;
typedef unsigned char BYTE;
typedef unsigned int UI;

typedef struct Block {
	BYTE data[BLOCK_SIZE];
} Blk;
