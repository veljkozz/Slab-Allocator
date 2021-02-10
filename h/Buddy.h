#pragma once
#include "preComp.h"


typedef struct BuddyBlock {
	struct BuddyBlock* next;
	short order;
} buddyBlk;

typedef struct BuddyAllocator {
	buddyBlk* free[16];	// list of free blocks
						// this would give us maximum of 2^15*BLOCK_SIZE(2^12) = 2^27 = 128MB, 
						// which is more than enough
	int highestOrder;
	int numBlocks;
	int numFreeBlocks;
	void* memstart;
} buddyAllocator;

buddyAllocator* buddyInit(void* addr, UL numBlocks);
buddyBlk *buddyAlloc(buddyAllocator* buddy, UL numBlock);
void buddyDealloc(buddyAllocator* buddy, buddyBlk* block, ULL size);
void buddyPrint(buddyAllocator* buddy);
