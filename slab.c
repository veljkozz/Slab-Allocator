#define _CRT_SECURE_NO_WARNINGS
#include "Buddy.h"
#include "slab.h"
#include "preComp.h"
#include "windows.h"
#include <stdio.h>

#define MAX_CHAR 30

CRITICAL_SECTION lck;

typedef enum ERROR_CODES {
	NO_ERR = 0,
	OUT_OF_MEM,
	OBJ_NOT_FOUND
} Error;

typedef struct SlabNode {
	void* mem;
	struct SlabNode* next;
	struct SlabNode* prev;
	kmem_cache_t* cache;
	ULL cacheLineOffs;
	ULL numObjects;
	ULL numFree;

	void* blkStart;
	BYTE flags[512]; // this gives us 512x8 = 4096 possible objects per slab, which is more than enough
} SlabNode;

typedef struct kmem_cache_s {
	struct kmem_cache_s* next;
	struct kmem_cache_s* prev;
	// slab allocator first tries to satisfy request by alloc in a partial slab
	SlabNode* partial;
	// if such can't be found it tries to find an empty slab
	SlabNode* empty;
	// if there is no empty slab a new one is allocated in contiguous memory
	SlabNode* full;

	void(*ctor)(void*);
	void(*dtor)(void*);
	ULL mem_unused;
	char name[MAX_CHAR];
	ULL numObj;	  // num of obj per slab
	ULL objSize;  
	ULL numSlabs;
	ULL slabSize; // in blocks
	ULL LineOffs;

	Error Error;  

	short slabStrategy; // 0 - allocate slab metadata on beggining of block, shared with objects   
						// 1 - allocate 1 separate block for slab metadata

	CRITICAL_SECTION lck;
} kmem_cache_t;

typedef struct SlabAllocator {
	buddyAllocator* buddy;
	kmem_cache_t* caches;
	kmem_cache_t* buffers[13];  // small mem buffers
} SlabAllocator;

SlabAllocator* SlabAlloc = 0;


void slab_add(SlabNode** head, SlabNode* slab) {
	if (slab) {
		slab->prev = 0;
		slab->next = *head;
		if (*head != 0)(*head)->prev = slab;
		*head = slab;
	}
}

void slab_remove(SlabNode** head, SlabNode* slab) {
	if (slab) {
		SlabNode* prev = slab->prev;
		SlabNode* next = slab->next;
		if (prev) prev->next = next;
		else *head = (*head)->next;
		if (next) next->prev = prev;
	}
}

SlabNode* allocateSlab(kmem_cache_t* cache) {
	SlabNode* slab = 0;
	void* pmem = 0;
	ULL mem;
	if (cache->slabStrategy == 0) {
		pmem = buddyAlloc(SlabAlloc->buddy, cache->slabSize);
		if (pmem == 0) {
			cache->Error = OUT_OF_MEM;
			return 0;
		}
		slab = (SlabNode*)((ULL)pmem + cache->LineOffs);
		slab->blkStart = pmem;
		pmem = (void*)((ULL)pmem + cache->LineOffs);
		mem = ((ULL)pmem + sizeof(SlabNode));
		slab->mem = (void*)mem;
	}
	else {
		slab = buddyAlloc(SlabAlloc->buddy, 1);
		pmem = buddyAlloc(SlabAlloc->buddy, cache->slabSize - 1);
		if (pmem == 0 || slab == 0) {
			cache->Error = OUT_OF_MEM;
			return 0;
		}
		slab->blkStart = pmem;
		slab->mem = (void*)((ULL)pmem + cache->LineOffs);
		mem = slab->mem;
	}
	slab->cache = cache;
	slab->numObjects = cache->numObj;
	slab->numFree = slab->numObjects;
	slab->cacheLineOffs = cache->LineOffs;
	cache->LineOffs += CACHE_L1_LINE_SIZE;
	if (cache->LineOffs >= cache->mem_unused) cache->LineOffs = 0;
	if (cache->ctor) {
		for (int i = 0; i < cache->numObj; ++i) {
			cache->ctor((void*)mem);
			mem += cache->objSize;
		}
	}
	for (int i = 0; i <= (cache->numObj - 1) / 8; ++i) {
		slab->flags[i] = (BYTE)0;
	}
	cache->numSlabs++;
	return slab;
}

void deallocateSlab(kmem_cache_t* cache, SlabNode* slab) {
	if (cache->dtor != 0) {
		ULL mem = slab->mem;
		for (int i = 0; i < cache->numObj; i++) {
			cache->dtor((void*)mem);
			mem += cache->objSize;
		}
	}
	if(cache->slabStrategy == 0) buddyDealloc(SlabAlloc->buddy, slab->blkStart, cache->slabSize);
	else {
		buddyDealloc(SlabAlloc->buddy, slab->blkStart, cache->slabSize - 1);
		buddyDealloc(SlabAlloc->buddy, (void*)slab, 1);
	}
	cache->numSlabs--;
}


void kmem_add(kmem_cache_t ** head, kmem_cache_t *cache) {
	cache->next = *head;
	cache->prev = 0;
	if (*head != 0) {
		(*head)->prev = cache;
	}
	*head = cache;
}
void kmem_remove(kmem_cache_t **head, kmem_cache_t *cache) {
	if (*head != 0 && cache != 0) {
		kmem_cache_t *prev = cache->prev;
		kmem_cache_t *next = cache->next;
		if (prev) prev->next = next;
		else *head = next;
		if (next) next->prev = prev;
	}
}

void kmem_init(void * space, int block_num)
{
	InitializeCriticalSection(&lck);
	EnterCriticalSection(&lck);
	buddyAllocator* buddy = buddyInit(space, block_num);
	SlabAlloc = (SlabAllocator*)buddyAlloc(buddy,1);
	SlabAlloc->buddy = buddy;
	SlabAlloc->caches = 0;
	const char*buffNames[13] = { "buff1", "buff2", "buff3", "buff4", "buff5", "buff6",
		"buff7", "buff8", "buff9", "buff10", "buff11", "buff12", "buff13" };
	for (int i = 0; i < 13; i++) {
		SlabAlloc->buffers[i] = kmem_cache_create(buffNames[i], pow2(5 + i), 0, 0);
		kmem_remove(&SlabAlloc->caches, SlabAlloc->buffers[i]);
		SlabAlloc->buffers[i]->next = 0;
		SlabAlloc->buffers[i]->prev = 0;
		int test = 1;
	}
	LeaveCriticalSection(&lck);
}


kmem_cache_t * kmem_cache_create(const char * name, size_t size, void(*ctor)(void *), void(*dtor)(void *))
{
	EnterCriticalSection(&lck);
	kmem_cache_t *cache = (kmem_cache_t*)buddyAlloc(SlabAlloc->buddy, 1);
	if (cache == 0) {
		printf("BuddyAllocator failed");
		LeaveCriticalSection(&lck);
		return 0;
	}
	LeaveCriticalSection(&lck);

	InitializeCriticalSection(&cache->lck);
	EnterCriticalSection(&cache->lck);
	cache->objSize = size;
	strncpy(cache->name, name, 30);
	cache->ctor = ctor;
	cache->dtor = dtor;
	cache->full = 0;
	cache->partial = 0;
	cache->empty = 0;
	cache->Error = NO_ERR;
	ULL blkss = BLOCK_SIZE;
	
	int i = 1;
	while (size > blkss) {
		blkss <<= 1;
	}
	ULL sizeOfSlabMetaData = sizeof(SlabNode);
	// 2 TYPES OF STRATEGY - Slab metadata is allocated on beginning of continuous memory
	//					   - Slab metadata gets 1 separate block allocated for it
	ULL actualSize = blkss - sizeof(SlabNode);
	cache->slabSize = blkss / BLOCK_SIZE;	// blkss - memory of blocks needed for slab
	ULL numObj = actualSize / size;			
	if (numObj == 0) {
		cache->slabStrategy = 1;
		actualSize = blkss;
		cache->slabSize = blkss / BLOCK_SIZE + 1;
		numObj = actualSize / size;
	}
	else cache->slabStrategy = 0;

	cache->numObj = numObj;
	cache->mem_unused = actualSize - numObj * size;
	
	cache->numSlabs = 0;
	cache->LineOffs = 0;
	kmem_add(&SlabAlloc->caches, cache);
	LeaveCriticalSection(&cache->lck);
	return cache;
}

int kmem_cache_shrink(kmem_cache_t * cachep)
{
	EnterCriticalSection(&cachep->lck);
	int blkFree = 0;
	for (SlabNode* slab = cachep->empty; slab != 0; ) {
		SlabNode* oldSlab = slab;
		slab = slab->next;
		blkFree += cachep->slabSize;
		deallocateSlab(cachep, oldSlab);
	}
	cachep->empty = 0;
	LeaveCriticalSection(&cachep->lck);
	return blkFree;
}

BOOL checkbit(BYTE byte, short bit) {
	return byte & (1 << bit);
}
void setbit(BYTE* byte, short bit) {
	*byte |= (1 << bit);
}
void clearbit(BYTE* byte, short bit) {
	*byte &= (~(BYTE)(1 << bit));
}
void * kmem_cache_alloc(kmem_cache_t * cachep)
{
	EnterCriticalSection(&cachep->lck);
	// first try to find space in partial, then in empty, if none found create new slab
	void *pret = 0;

	if (cachep->empty == 0 && cachep->partial == 0) { // allocate new slab
		SlabNode* newSlab = allocateSlab(cachep);
		if (newSlab == 0 || newSlab->mem == 0) {
			cachep->Error = OUT_OF_MEM;
			LeaveCriticalSection(&cachep->lck);
			return 0; //error
		}
		pret = newSlab->mem;
		newSlab->flags[0] = (BYTE)1;
		if (--newSlab->numFree == 0) slab_add(&cachep->full, newSlab);
		else slab_add(&cachep->partial, newSlab);
	}
	else if (cachep->partial != 0) {
		SlabNode* ptr = cachep->partial;
		for (int i = 0; i < ptr->numObjects; i++) {
			if (!checkbit(ptr->flags[i / 8], i % 8)) {
				setbit(&ptr->flags[i / 8], i % 8);
				pret = (void*)((ULL)ptr->mem + ptr->cache->objSize*i);
				if (--ptr->numFree == 0) {
					slab_remove(&cachep->partial, ptr);
					slab_add(&cachep->full, ptr);
				}
				break;
			}
		}
	}
	else if (cachep->empty != 0) {
		SlabNode* slab = cachep->empty;
		slab_remove(&cachep->empty, slab);
		slab->numFree = cachep->numObj;
		if (--slab->numFree == 0) slab_add(&cachep->full, slab);
		else slab_add(&cachep->partial, slab);
		pret = slab->mem;
		slab->flags[0] = (BYTE)1;
	}
	LeaveCriticalSection(&cachep->lck);
	return pret;
}

BOOL isIn(void* objp, SlabNode* pSlab, kmem_cache_t* pcache) {
	return pSlab->mem <= objp && (ULL)pSlab->mem + pcache->objSize*pcache->numObj >= (ULL)objp;
}

void kmem_cache_free(kmem_cache_t * cachep, void * objp)
{
	EnterCriticalSection(&cachep->lck);
	ULL objemm = (ULL)objp;
	SlabNode* ptr = cachep->partial;
	while (ptr && !isIn(objp, ptr, cachep)) ptr = ptr->next;
	int inPartial = 1;
	if (ptr == 0) {
		inPartial = 0;
		ptr = cachep->full;
		while (ptr && !isIn(objp, ptr, cachep)) ptr = ptr->next;
	}
	
	if (ptr == 0) {
		cachep->Error = OBJ_NOT_FOUND; // error
	}
	else {
		int i = 0;
		while ((ULL)ptr->mem + i * cachep->objSize != (ULL)objp) i++;

		if (!checkbit(ptr->flags[i / 8], i % 8) ) {
			printf("Already deallocated!\n");
			return;
		}

		ptr->numFree++;
		clearbit(&ptr->flags[i / 8], i % 8);
		if (ptr->numFree == cachep->numObj) {
			if (inPartial) slab_remove(&cachep->partial, ptr);
			else slab_remove(&cachep->full, ptr);
			slab_add(&cachep->empty, ptr);
		}
	}
	LeaveCriticalSection(&cachep->lck);
}


void * kmalloc(size_t size)
{
	short order2 = getOrder2(size);
	if (order2 < 5) order2 = 5;
	if (order2 > 17) order2 = 17;
	void* pret = kmem_cache_alloc(SlabAlloc->buffers[order2-5]);
	if (pret == 0) {
		SlabAlloc->buffers[order2 - 5]->Error = OUT_OF_MEM;
	}
	return pret;
}

void kfree(const void * objp)
{
	EnterCriticalSection(&lck);
	for (int i = 0; i < 13; i++) {
		SlabNode* slab = SlabAlloc->buffers[i]->partial;
		while (slab) {
			if (isIn(objp, slab, SlabAlloc->buffers[i])) break;
			else slab = slab->next;
		}
		if (slab != 0) {
			kmem_cache_free(SlabAlloc->buffers[i], objp);
		}
		else {
			slab = SlabAlloc->buffers[i]->full;
			while (slab) {
				ULL ULLobjp = (ULL)objp;
				ULL memStart = (ULL)slab->mem;
				ULL memEnd = (ULL)slab->mem + slab->cache->objSize;
				void* pmemEnd = (void*)memEnd;
				if (isIn(objp, slab, SlabAlloc->buffers[i])) break;
				else slab = slab->next;
			}
			if (slab != 0) {
				kmem_cache_free(SlabAlloc->buffers[i], objp);
			}
			else {
				//object not in this buffer
			}
		}
	}
	LeaveCriticalSection(&lck);
}

void kmem_cache_destroy(kmem_cache_t * cachep)
{
	EnterCriticalSection(&cachep->lck);
	
	SlabNode* slab = cachep->partial;
	while (slab != 0) {
		SlabNode* old = slab;
		slab = slab->next;
		slab_remove(&cachep->partial, old);
		deallocateSlab(cachep, old);
	}
	slab = cachep->full;
	while (slab != 0) {
		SlabNode* old = slab;
		slab = slab->next;
		slab_remove(&cachep->full, old);
		deallocateSlab(cachep, old);
	}
	kmem_cache_shrink(cachep);
	kmem_remove(&SlabAlloc->caches, cachep);
	buddyDealloc(SlabAlloc->buddy, cachep, 1);
	LeaveCriticalSection(&cachep->lck);
}

void kmem_cache_info(kmem_cache_t * cachep)
{
	EnterCriticalSection(&lck);
	printf("------------------------------------\n");
	printf("Cache: %s\n", cachep->name);
	printf("\tObject size: %llu B\n", cachep->objSize);
	printf("\tCache size: %llu blocks,", cachep->numSlabs*cachep->slabSize);
	printf("\tNum of slabs: %llu\n", cachep->numSlabs);
	printf("\tNum of obj per slab: %llu\n", cachep->numObj);
	
	ULL numSlabs = 0;
	ULL numObjs = 0;
	for (SlabNode* t = cachep->partial; t != 0; t = t->next) {
		numSlabs++;
		numObjs += t->numObjects;
	}
	for (SlabNode* t = cachep->full; t != 0; t = t->next) {
		numSlabs++;
		numObjs += t->numObjects;
	}
	ULL sizeAll = numSlabs * cachep->slabSize * BLOCK_SIZE;
	ULL sizeUsed = numObjs * cachep->objSize;
	if (numSlabs > 0) {
		printf("\tPopunjenost: %f %% \n", (sizeUsed / ((double)numSlabs*cachep->numObj*cachep->objSize))* 100.);
		printf("\tIskoriscenje: %f %% \n", (sizeUsed / (double)sizeAll)* 100.);
	}
	else {
		printf("\tPopunjenost: 0 %% \n", (sizeUsed / (double)numSlabs*cachep->numObj*cachep->objSize)* 100.);
		printf("\tIskoriscenje: 0 %% \n", (sizeUsed / (double)sizeAll)* 100.);
	}
	printf("-------------------------------------\n");

	LeaveCriticalSection(&lck);
}

int kmem_cache_error(kmem_cache_t * cachep)
{
	switch(cachep->Error){
	case NO_ERROR:
		printf("No error\n");
		return 0;
	case OUT_OF_MEM:
		printf("Error: OUT OF MEM\n");
		return OUT_OF_MEM;
	case OBJ_NOT_FOUND:
		printf("Error: OBJECT NOT FOUND\n");
		return OBJ_NOT_FOUND;
	default:
		return 0;
	}
	
}
