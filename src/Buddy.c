#include "Buddy.h"
#include "preComp.h"
#include <Windows.h>
#include <stdio.h>
CRITICAL_SECTION crit_sec;

// util functions
short getOrder2(UL n) {
	short i = 0;
	while ((1 << i) < n) i++;
	return i;
}

ULL pow2(short i) {
	return (1 << i);
}

void buddyInsert(buddyBlk** head, buddyBlk* inserting);
void buddyRemove(buddyBlk** head, buddyBlk* removing);
void split(buddyAllocator* buddy, buddyBlk* splitting, short order);
void mergeAll(buddyAllocator* buddy, short order);

buddyAllocator * buddyInit(void * addr, UL numBlocks)
{
	InitializeCriticalSection(&crit_sec);
	EnterCriticalSection(&crit_sec);

	// buddy allocator metadata allocated at beginning of given memory
	ULL memEnd = ((ULL)addr + numBlocks * BLOCK_SIZE);
	ULL modEnd = memEnd % BLOCK_SIZE;
	ULL memStart = (ULL)addr;
	ULL modStart = (ULL)addr % BLOCK_SIZE;
	addr = ((ULL)addr + sizeof(buddyAllocator));
	addr = (void*)(memStart + BLOCK_SIZE - (modStart == 0 ? BLOCK_SIZE : modStart)); // addr aligned to block
	buddyAllocator* buddyAlloc = (buddyAllocator*)memStart;
	addr = (void*)((ULL)addr + BLOCK_SIZE);
	ULL freeMemSize = (memEnd - modEnd) - (ULL)addr;
	buddyAlloc->highestOrder = 12;
	while (pow2(buddyAlloc->highestOrder) < freeMemSize) buddyAlloc->highestOrder++;
	buddyAlloc->highestOrder -= 12;
	buddyAlloc->numBlocks = freeMemSize / BLOCK_SIZE;
	buddyAlloc->numFreeBlocks = buddyAlloc->numBlocks;
	ULL currmem = (ULL)addr;
	for (int i = 0; i < 16; i++) buddyAlloc->free[i] = 0;
	for (int i = 0; i < buddyAlloc->numBlocks; ++i) {
		buddyInsert(&buddyAlloc->free[0], (buddyBlk*)currmem);
		currmem += BLOCK_SIZE;
	}
	buddyAlloc->memstart = addr;
	mergeAll(buddyAlloc,0);
	LeaveCriticalSection(&crit_sec);
	return buddyAlloc;
}

buddyBlk *buddyAlloc(buddyAllocator* buddy, UL numBlock) {
	EnterCriticalSection(&crit_sec);
	if (numBlock > buddy->numFreeBlocks) {
		printf("BUDDY OUT OF MEM\n");
		LeaveCriticalSection(&crit_sec);
		return 0;
	}
	short i = getOrder2(numBlock);
	buddyBlk* retBlk = 0;
	if (buddy->free[i] != 0) {
		retBlk = buddy->free[i];
		buddyRemove(&buddy->free[i], retBlk);
		
	}
	else {
		int cnt;
		for (cnt = i+1; cnt < 16; ++cnt) {
			if (buddy->free[cnt] != 0) {
				split(buddy, buddy->free[cnt], i);
				break;
			}
		}
		if (buddy->free[i] != 0){
			retBlk = buddy->free[i];
			buddyRemove(&buddy->free[i], retBlk);
		}
	}
	if (retBlk != 0) {
		buddy->numFreeBlocks -= numBlock;
	}
	if (retBlk == 0) {
		printf("BUDDY OUT OF MEM\n");
	}
	LeaveCriticalSection(&crit_sec);
	return retBlk;
}
void buddyDealloc(buddyAllocator * buddy, buddyBlk * blk, ULL numBlock)
{
	EnterCriticalSection(&crit_sec);
	short i = getOrder2(numBlock);
	blk->order = i;
	buddyInsert(&buddy->free[i], blk);
	buddy->numFreeBlocks += numBlock;
	mergeAll(buddy, i);
	LeaveCriticalSection(&crit_sec);
}

void split(buddyAllocator* buddy, buddyBlk* splitting, short order) {
	if (splitting->order > order) {
		ULL size = pow2(splitting->order)*BLOCK_SIZE;
		buddyBlk * b1 = splitting;
		buddyBlk* b2 = (buddyBlk*)((ULL)splitting + (size >> 1));
		buddyRemove(&buddy->free[splitting->order], splitting);
		b1->order = splitting->order - 1;
		b2->order = b1->order;
		buddyInsert(&buddy->free[b1->order], b1);
		buddyInsert(&buddy->free[b2->order], b2);
		split(buddy, b1, order);
	}
}

void buddyInsert(buddyBlk** head, buddyBlk* inserting) {
	if (head == 0) {
		inserting->next = 0;
		*head = inserting;
		if (head == 0) printf("Ijaoo\n");
		return;
	}
	buddyBlk* ptr = *head;
	if (ptr == 0 || inserting < ptr) {
		inserting->next = ptr;
		*head = inserting;
		return;
	}
	while (ptr->next && ptr->next < inserting) ptr = ptr->next;
	inserting->next = ptr->next;
	ptr->next = inserting;
}
void buddyRemove(buddyBlk** head, buddyBlk* removing) {
	buddyBlk* ptr = *head;
	if (ptr == 0) return;
	if (ptr == removing) {
		*head = (*head)->next;
		return;
	}
	while (ptr->next && ptr->next < removing) ptr = ptr->next;
	ptr->next = removing->next;
}
void mergeAll(buddyAllocator* buddy, short order) {
	
	for (short i = order; i < 16; ++i) {
		BOOL keepMerging = FALSE;
		ULL len = 1 << (i);
		buddyBlk* ptr = buddy->free[i];
		while (ptr != 0 && ptr->next != 0) {
			ULL ul1 = (ULL)ptr + len * BLOCK_SIZE;
			ULL ul2 = (ULL)(ptr->next);
			ULL offs = (ULL)ptr - (ULL)buddy->memstart;
			ULL size = pow2(i);
			if ((offs & size) == 0) {
				if (ul1 == ul2) { // merge them
					buddyBlk* newHead = ptr->next->next;
					buddyRemove(&buddy->free[i], ptr);
					buddyRemove(&buddy->free[i], ptr->next);
					buddyInsert(&buddy->free[i + 1], ptr);
					ptr->order = i + 1;
					ptr = newHead;
					keepMerging = TRUE;
				}
				else ptr = ptr->next;
			}
			else ptr = ptr->next;
			
		}
		if (keepMerging == FALSE) break;
	}
}

void buddyPrint(buddyAllocator * buddy)
{
	EnterCriticalSection(&crit_sec);
	for (int i = 0; i < 16; i++) {
		printf("%d: ", i);
		for (buddyBlk* ptr = buddy->free[i]; ptr != 0; ptr = ptr->next) {
			printf("%d ", ((ULL)ptr - (ULL)buddy->memstart)/BLOCK_SIZE);
		}
		printf("\n");
	}
	LeaveCriticalSection(&crit_sec);
}
