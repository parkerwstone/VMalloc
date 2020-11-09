/*
 * final.c - memory allocation functions (warning: not thread safe)
 *
 * General usage:
 *  1. call VInit with max memory usage
 *  2. call VMalloc to suballocate memory created from step 1
 *  3. call VFree to free up the memory allocated from step 2
 *  4. call VPut to allocate and put data into memory
 *  5. call VGet to retrieve and deallocate data from memory
 *
 * Memory is suballocated using metadata block to describe suballocated memory.
 * Metadata block will precede the suballocation block. Calls to VMalloc will
 * return a reference to the address to the suballocated memory block
 * (the address immediately after the metadata block).
 *
 * Note: memory is allocated on 8 byte boundaries. Requested allocations are
 * padded up to the nearest 8 byte boudary, if needed.
 *
 * Metadata block is kept before the memory block. It keeps track of the memory
 * block's size and padding.
 *
 * +-----------------------------------------------------------------------+
 * | metadata|<user memory>|metadata|<more user memory>|metadata|<free mem>|
 * +-----------------------------------------------------------------------+
 *           ^                      ^
 *           |                      |
 *        user ptr               user ptr
 *
 * VMalloc
 * =======
 * The VMalloc scheme is a level of virtualization between the pointer returned
 * to the caller and the pointer to the actual memory block. The pointer returned
 * to the caller will never change, however the pointer to the actual memory block
 * will change. This allows us to move memory around with affecting the calling
 * program.
 *
 * Allocating memory is simplified by the free memory scheme (see below),
 * since there is one and only one free memory block.
 *
 * VFree
 * ======
 * Free memory is maintained in a single block in the right most area of memory.
 * When a used block is freed, all memory to the right of that freed block will
 * be shifted to the left by the size of that freed block. Here are the steps
 * taken during a call to VFree() where the compaction of memory is done in
 * compact().
 *
 * +-----------------------------------------------------------------------+
 * | metadata|<user memory>|metadata|<more user memory>|metadata|<free mem>|
 * +-----------------------------------------------------------------------+
 *                ^ free this
 *
 * Free the memory block:
 * +-----------------------------------------------------------------------+
 * | <free>                |metadata|<more user memory>|metadata|<free mem>|
 * +-----------------------------------------------------------------------+
 *
 * Shift memory to the left:
 * +-----------------------------------------------------------------------+
 * | <free>                |metadata|<more user memory>|metadata|<free mem>|
 * +-----------------------------------------------------------------------+
 * <-----------------------|...............................................|
 *
 * +-----------------------------------------------------------------------+
 * |metadata|<more user memory>|metadata|<free mem>|
 * +-----------------------------------------------------------------------+
 *
 * Update the free size by the amount of memory freed up:
 * +-----------------------------------------------------------------------+
 * |metadata|<more user memory>|metadata|<free mem>|
 * +-----------------------------------------------------------------------+
 *                                                 |----------------------->
 *
 * +-----------------------------------------------------------------------+
 * |metadata|<more user memory>|metadata|           <free mem>             |
 * +-----------------------------------------------------------------------+
 *
 * Virtualized pointer scheme
 * ==========================
 * The virtualized memory pointer scheme is maintained externally from the managed
 * memory block using a linked list of pointer_node_structs (aka pointer_node).
 * A pointer_node contains a pointer to a memory block (called addr) and a
 * pointer to the next pointer_node. The address of addr is returned to the caller
 * giving the caller a virtualized reference to memory. The caller needs to dereference
 * this pointer in order to access its memory block.
 *
 * The head node referred to as ptrListHead is a dummy node. The tail node of
 * the list referred to as ptrListTail is always the reference to the free block.
 *
 * Tools
 * =====
 * There are two debugging/diagnostic functions: heapDump() (use HEAP_DUMP)
 * and printHeapStats() (use PRINT_HEAP_STATS). heapDump() will walk all of the
 * memory blocks and print out their addresses, sizes and paddings.
 * printHeapStats() outputs statistical information about VMalloc and VFree's
 * performance.
 *
 *  Created on: Nov 23, 2019
 *      Author: Parker Stone
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// limits.h needed for tests
#include <limits.h>

// force 8 byte packing
#pragma pack(8)

typedef char *addrs_t;
typedef void *any_t;

#define TRUE 1
#define FALSE 0
#define MIN_SIZE 8
#define DEFAULT_MEM_SIZE 1 << 20
// used for clock cycles
#define rdtsc(x)      __asm__ __volatile__("rdtsc \n\t" : "=A" (*(x)))
// gives us a pointer to the very end of the allocated memory block
#define END_OF_ALLOCATED_MEMORY (baseptr + memsize)
// convenience macro for moving a metadata pointer by distance in bytes
#define MOVE_METADATA_PTR(pMetadata,distance) (metadata *)(((void *)pMetadata) + distance);
// convenience macro for moving an addr pointer by distance in bytes
#define MOVE_ADDR_PTR(addr,distance) (addrs_t)(((void *)addr) + distance);
// convenience for getting the metadata pointer from a user addrs_t
#define GET_METADATA(addr) (metadata *)(*((addrs_t *) (addr)) - sizeof(metadata))
// this returns a metadata pointer to the free memory block
#define FREE_METADATA GET_METADATA(&ptrListTail -> addr)
// convenience macros for calling tools and these can be optionally redefined to
// nothing for silent execution
#define PRINT_HEAP_STATS printHeapStats()
#define HEAP_DUMP heapDump()
// #define HEAP_DUMP

// points to the beginning of the memory block, allocated by the system malloc
addrs_t baseptr;
// user specified memsize given to VInit
size_t memsize;

// this is the pointer_node definition for the linked list that maintains the virtual
// memory pointers
typedef struct pointer_node_struct {
    // this is the pointer to the memory block, the address of this is returned
    // to the caller
    addrs_t addr;
    struct pointer_node_struct *pNext;
} pointer_node;
pointer_node *ptrListHead, // dummy node
             *ptrListTail; // always points to the free block

struct metadata_struct;
typedef struct metadata_struct metadata;

// this is the metadata that preceeds every block of suballocated memory
struct metadata_struct{
    size_t size;     // user specified size + 8 byte alignment padding, does not include the size of metadata
    int padding;     // amount of padding applied, used for stats only
};

// convenience container for some of the stats required for heap stats reporting
struct stats_struct {
    long numMallocRequests;        // number of times vmalloc is called
    long numFreeRequests;          // number of times vfree is called
    long numFailures;              // number of times an error occurs
    unsigned long totalMallocTime; // total amount of time vmalloc required to execute
    unsigned long totalFreeTime;   // total amount of time vfree required to execute
};

// instance of the stats, used for heap stats reporting
struct stats_struct stats;

size_t roundTo8Byte(size_t size);
int isValidAddress(addrs_t addr);
void compact(metadata *pMetadata);
metadata *Alloc(size_t size);
void printHeapStats(void);
pointer_node *createPointerNode(void);
pointer_node *insertPointerNode(pointer_node *pNode);
pointer_node *getPriorPointerNode(addrs_t *addr);
void freeNextNode(pointer_node *pNode);
void heapDump(void);

// This initializes and allocates system memory of size for later suballocation
// with VMalloc(). The size will be adjusted up to the nearest 8 byte boundary.
// If there is not enough system memory, no allocation will occur.
// This should only be called once with the maximum memory size to use, before
// any calls to VMalloc() or VFree(). If this is called a second time, that is
// considered an error, but it is harmless since no reinitialization occurs,
// the memory from the first VInit call is preserved. When finished using this memory,
// call releaseMemory() to free up allocated system
// memory. VInit can be called after releaseMemory() is called.
void VInit (size_t size) {
    metadata *pFreeMetadata;
    // adjust given size up to 8 byte boundary, if needeed
    size_t adjustedSize = roundTo8Byte(size);
    // check to see if we are being reinitialized
    if (baseptr != NULL) {
        printf("WARNING: cannot reinitialize\n");
        return;
    }
    // make sure requested size meets minimum criteria
    if (adjustedSize < sizeof(metadata) + MIN_SIZE) {
        fprintf(stderr, "ERROR: requested memory %zu is less than the minimum memory %zu\n",
                size, sizeof(metadata) + MIN_SIZE);
        return;
    }
    memsize = adjustedSize;
    // call system malloc to get a block of memory from the system
    baseptr = (addrs_t) malloc (adjustedSize);
    // check that malloc succeeded
    if (baseptr == NULL) {
        fprintf(stderr, "ERROR: unable to allocate block memory of size %zu\n", adjustedSize);
        return;
    }
    
    // set up the initial block's metadata
    pFreeMetadata = (metadata *) baseptr;
    pFreeMetadata -> size = adjustedSize - sizeof(metadata);
    pFreeMetadata -> padding = (int) (adjustedSize - size);
    
    // create linked list for maintaining the virtualized pointers
    ptrListHead = createPointerNode();
    // create the tail of the linked list, this will always be the reference to
    // free memory block
    ptrListTail = createPointerNode();
    ptrListHead -> pNext = ptrListTail;
    ptrListTail -> addr = MOVE_ADDR_PTR(pFreeMetadata, sizeof(metadata));
    
    // initialize stats
    stats.numMallocRequests = 0L;
    stats.numFreeRequests = 0L;
    stats.numFailures = 0L;
    stats.totalMallocTime = 0L;
    stats.totalFreeTime = 0L;
}

// Suballocate memory of "size," and return an addrs_t* to the reference of the
// newly suballocated block of memory. Return NULL if VInit() has not been called,
// there is no free memory of the size requested, or the size is less than minimum size.
// Note: memory allocations are done on 8 byte boundary, so the final suballocated
// block may be larger than "size."
// Memory allocated using this function can be freed with using VFree().
addrs_t *VMalloc(size_t size) {
    metadata *pCurrent;
    pointer_node *pNode;
    metadata *pFreeMetadata;
    size_t freeSize;
    unsigned long start, finish;
    
    // adjust size up to nearest 8 byte boundary if needed
    size_t adjustedSize = roundTo8Byte(size);
    stats.numMallocRequests++;
    rdtsc(&start);
    
    // check to see if Init() has been called
    if (baseptr == NULL) {
        printf("no block memory allocated, call VInit() first\n");
        stats.numFailures++;
        return NULL;
    }
    // get a metadata pointer to the free block
    pFreeMetadata = FREE_METADATA;
    // check to see memory if we can accomadate size
    if (pFreeMetadata -> size < adjustedSize + sizeof(metadata) + MIN_SIZE) {
        fprintf(stderr, "ERROR: insufficient memory for size: %zu\n", size);
        stats.numFailures++;
        return NULL;
    }
    // new allocation will occur where the pointer to the free metadata is
    // pointing to
    pCurrent = pFreeMetadata;
    // calculate the new free size (the size of free after the allocation occurs)
    freeSize = pFreeMetadata -> size - adjustedSize - sizeof(metadata);
    pCurrent -> padding = (int) (adjustedSize - size);
    // prepare the new memory block for use
    pCurrent -> size = adjustedSize;
    
    // create an entry in the linked list for this new memory block
    pNode = insertPointerNode(createPointerNode());
    // point a pointer_node's address to the new memory location
    pNode -> addr = MOVE_ADDR_PTR(pCurrent, sizeof(metadata));

    // move the memory location in ptrListTail to the new free memory location
    ptrListTail -> addr = MOVE_ADDR_PTR(pCurrent, pCurrent -> size + (2 * sizeof(metadata)));
    // update the metadata block for the free memory
    pFreeMetadata = FREE_METADATA;
    pFreeMetadata -> size = freeSize;
    pFreeMetadata -> padding = 0;
    
    // stop timer
    rdtsc(&finish);
    stats.totalMallocTime += finish - start;
    PRINT_HEAP_STATS;
    // return the address of the address (the virtual pointer)
    // to memory which is the address right after the metadata
    return &pNode -> addr;
}

// Free suballocated memory from VMalloc() calls. This could fail if VInit() has
// not been called or the given "addr" is not a valid address.
void VFree (addrs_t *addr) {
    metadata *pMetadata;
    size_t sizeToBeFreed;
    pointer_node *prevNode;
    unsigned long start, finish;
    metadata *pFreeMetadata;
    
    // start free timer
    rdtsc(&start);
    stats.numFreeRequests++;
    // check to see if VInit() has been called
    if (baseptr == NULL) {
        printf("ERROR: no block memory allocated, call VInit() first\n");
        stats.numFailures++;
        return;
    }
    // get the node in the linked list just prior to the node that contains addr
    prevNode = getPriorPointerNode(addr);
    // if the previous node is not found, then addr is invalid
    if (prevNode == NULL) {
        printf("ERROR: invalid address\n");
        stats.numFailures++;
        return;
    }
    // get the metadata pointer for addr
    pMetadata = GET_METADATA(addr);
    // calculate the size of memory to be freed
    sizeToBeFreed = pMetadata -> size + sizeof(metadata);
    
    // remove addr's node from the linked list
    freeNextNode(prevNode);
    
    // shift all memory to the right of addr to the left by the amount of
    // sizeToBeFreed
    compact(pMetadata);
    
    // shift left all addr pointers by sizeToBeFreed in the linked list for
    // addrs that are to the right of the freed memory block
    for (pointer_node *ptr = ptrListHead -> pNext; ptr != NULL; ptr = ptr -> pNext) {
        if (ptr -> addr > *addr) {
            ptr -> addr -= sizeToBeFreed;
        }
    }
  
    // update the metadata block for free memory
    pFreeMetadata = FREE_METADATA;
    pFreeMetadata -> size += sizeToBeFreed;
    pFreeMetadata -> padding = 0;
    
    HEAP_DUMP;
    rdtsc(&finish);
    stats.totalFreeTime += finish - start;
    PRINT_HEAP_STATS;
}

// Allocates and writes data of given size into memory. Returns NULL if unable
// to allocate memory.
addrs_t *VPut(any_t data, size_t size) {
    // allocates memory for data
    addrs_t *ptr = VMalloc(size);
    // check to see if VMalloc() succeeded
    if (ptr == NULL) {
        fprintf(stderr, "ERROR: unable to allocate memory of size %zu\n", size);
        return NULL;
    }
    // copy data into the new allocated memory block
    memcpy(*ptr, data, size);
    return ptr;
}

// Retrieves and deallocates return_data from addr of size. This function fails
// if size is too small, addr is invalid, or size is too big.
void VGet(any_t return_data, addrs_t *addr, size_t size) {
    metadata *pMetadata;
    if (size == 0) {
        return;
    }
    // check to see if addr is a valid address
    if (getPriorPointerNode(addr) == NULL) {
        fprintf(stderr, "ERROR: unable to get, invalid source address\n");
        return;
    }
    // get a metadata pointer to addr's memory block
    pMetadata = GET_METADATA(addr);
    // validate that the requested size is not greater than the allocated memory size
    if (pMetadata -> size < size) {
        fprintf(stderr, "ERROR: size %zu is greater than the memory block size of %zu\n", size, pMetadata -> size);
        return;
    }
    // copy the data from the memory block to the return_data
    memcpy(return_data, *addr, size);
    // free up the memory block for addr
    VFree(addr);
}

// Creates a new pointer_node to be included in the linked list. Free up the returned
// node by calling freeNextNode().
pointer_node *createPointerNode() {
    pointer_node *ptr = malloc(sizeof(pointer_node));
    memset(ptr, 0, sizeof(pointer_node)); // clean memory
    return ptr;
}

// Inserts the given pNode immediately after ptrListHead in the linked list
pointer_node *insertPointerNode(pointer_node *pNode) {
    pNode -> pNext = ptrListHead -> pNext;
    ptrListHead -> pNext = pNode;
    if (ptrListTail == ptrListHead) {
        ptrListTail = pNode;
    }
    return pNode;
}

// Removes the given pNode from the linked list and frees up its memory
void freeNextNode(pointer_node *pNode) {
    pointer_node *ptrNodeToFree = pNode -> pNext;
    pNode -> pNext = ptrNodeToFree -> pNext;
    if (ptrListTail == ptrNodeToFree) {
        ptrListTail = pNode;
    }
    free(ptrNodeToFree);
}

// Left shifts memory by the size of the memory being freed up for all memory
// to the right of the block being pointed to by pMetadata.
// This ensures that the one and only one free memory block will be the right most
// memory block.
void compact(metadata *pMetadataToBeFreed) {
    // get a pointer to the metadata block of the block to the immediate right of
    // the block being freed
    metadata *pNextMetadata = MOVE_METADATA_PTR(pMetadataToBeFreed,pMetadataToBeFreed -> size + sizeof(metadata));
    // perform physical memory shift
    memmove(pMetadataToBeFreed, pNextMetadata, END_OF_ALLOCATED_MEMORY - (addrs_t) pNextMetadata);
}

// Returns the pointer_node immediately preceeding the node that contains addr.
// This will return NULL if addr is not found in the linked list.
pointer_node *getPriorPointerNode(addrs_t *addr) {
    pointer_node *pNode;
    if (addr == NULL) {
        return NULL;
    }
    for (pNode = ptrListHead;
         pNode != NULL && pNode -> pNext != NULL && &pNode -> pNext -> addr != addr;
         pNode = pNode -> pNext) {
        // loop until done
    }
    return pNode -> pNext == NULL ? NULL : pNode;
}

// return next 8 byte boundary if necessary
size_t roundTo8Byte(size_t size) {
    int remainder = size % 8;
    if (remainder == 0) {
        return size;
    }
    return size + (8 - remainder);
}

// walks through the memory block and prints out each metadata
void heapDump() {
    metadata *pMetadata;
    int numBlocksUsed = 0;
    int index = 1;
    metadata *pFreeMetadata;

    printf("heap dump:\n");
    if (ptrListHead == NULL) {
        printf("<memory released>\n");
        return;
    }
    pFreeMetadata = FREE_METADATA;
    for (pointer_node *ptr = ptrListHead -> pNext; ptr != NULL; ptr = ptr -> pNext) {
        pMetadata = GET_METADATA(&ptr -> addr);
        printf("(%d) address: %x, size: %zu, padding: %d\n",
               index,
               (unsigned int) ptr -> addr,
               pMetadata -> size,
               pMetadata -> padding);
        numBlocksUsed++;
        index++;
    }
    index--;
    printf("\n%d total blocks, used: %d blocks (%zu bytes), free: %zu bytes, %zu bytes used by metadata\n\n",
           index,
           numBlocksUsed,
           memsize - pFreeMetadata -> size,
           pFreeMetadata -> size,
           index * sizeof(metadata));
}

// This releases memory allocated in VInit(). It is safe to call VInit() again.
void releaseMemory() {
    if (baseptr == NULL) {
        return;
    }
    // free up allocated memory
    free(baseptr);
    baseptr = NULL;
    // free up memory used by linked list
    for (pointer_node *ptr = ptrListHead, *pNext = ptr -> pNext;
         ptr != NULL;
         ptr = pNext, pNext = (ptr == NULL ? NULL : ptr -> pNext)) {
        free(ptr);
    }
    ptrListHead = ptrListTail = NULL;
}

// prints the heaps statistics per project requirements
void printHeapStats() {
    metadata *pMetadata;
    metadata *pFreeMetadata;
    long numAllocatedBlocks = 0L;
    long numRawAllocatedBytes = 0L;
    long numPaddedTotalAllocatedBytes = 0L;
    long numPaddingBytes = 0L;
    int index = 1;
    
    if (baseptr == NULL) {
        return;
    }
    pFreeMetadata = FREE_METADATA;
    for (pointer_node *pNode = ptrListHead -> pNext; pNode != NULL; pNode = pNode -> pNext) {
        pMetadata = GET_METADATA(&pNode -> addr);
        numAllocatedBlocks++;
        numRawAllocatedBytes += (pMetadata -> size - pMetadata -> padding);
        numPaddedTotalAllocatedBytes += pMetadata -> size;
        numPaddingBytes += pMetadata -> padding;
    }
    printf("<<Part 2 for Region M2>>\n");
    printf("Number of allocated blocks : %ld\n", numAllocatedBlocks);
    printf("Number of free blocks  : %d\n", pFreeMetadata -> size == 0 ? 0 : 1);
    printf("Raw total number of bytes allocated : %ld\n", numRawAllocatedBytes);
    printf("Padded total number of bytes allocated : %ld\n", numPaddedTotalAllocatedBytes);
    printf("Raw total number of bytes free : %ld\n", pFreeMetadata -> size);
    printf("Aligned total number of bytes free : %ld \n", memsize - numPaddingBytes - (index * sizeof(metadata)));
    printf("Total number of VMalloc requests : %ld\n", stats.numMallocRequests);
    printf("Total number of VFree requests: %ld\n", stats.numFreeRequests);
    printf("Total number of request failures: %ld\n", stats.numFailures);
    printf("Average clock cycles for a VMalloc request: %.3f\n",
           stats.numMallocRequests == 0 ? 0.0 : (float) stats.totalMallocTime / (float) stats.numMallocRequests);
    printf("Average clock cycles for a VFree request: %.3f\n",
           stats.numFreeRequests == 0 ? 0.0 : (float) stats.totalFreeTime / (float) stats.numFreeRequests);
    printf("Total clock cycles for all requests: %lu\n", stats.totalMallocTime + stats.totalFreeTime);
}

// ========================== TESTS ===========================================
#define NUM_TESTS 10

int testFromAssignment(int mem_size) {
    int success = 1;
    int i, n;
    char s[80];
    addrs_t *addr1, *addr2;
    char data1[80];
    char data2[80];
    
    printf("Running testFromAssignment\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        fprintf(stderr, "ERROR: CANNOT INITIALIZE: basepointer is not null");
        success = 0;
    }

    for (i = 0; i < NUM_TESTS; i++) {
        n = sprintf (s, "String 1, the current count is %d\n", i);
        addr1 = VPut (s, n+1);
        heapDump();
        addr2 = VPut (s, n+1);
        heapDump();
        if (addr1 == addr2) {
            fprintf(stderr, "ERROR: address are equal\n");
            success = 0;
        }
        if (addr1)
          printf ("Data at %x is: %s", (unsigned int) *addr1, *addr1);
        if (addr2)
          printf ("Data at %x is: %s", (unsigned int) *addr2, *addr2);
        if (addr2) {
          VGet ((any_t)data2, addr2, n+1);
          heapDump();
        }
        if (addr1) {
          VGet ((any_t)data1, addr1, n+1);
          heapDump();
        }
        if (memcmp(&data1, &data2, n) != 0) {
            fprintf(stderr, "ERROR: data not equal\n");
            success = 0;
        } else {
            printf("data is equal\n");
        }
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testInit(int mem_size) {
    int success = 1;
    metadata *pFreeMetadata;
    printf("Running testInit\n");
    VInit (1000);
    heapDump();
    if (baseptr == NULL) {
        fprintf(stderr, "TEST FAILED: base pointer is null\n");
        success = 0;
    }
    pFreeMetadata = FREE_METADATA;
    if (pFreeMetadata == NULL) {
        fprintf(stderr, "TEST FAILED: block not marked as free\n");
        success = 0;
    }
    if (pFreeMetadata -> size != 1000 - sizeof(metadata)) {
        fprintf(stderr, "TEST FAILED: size is incorrect\n");
        success = 0;
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testDoubleInit(int mem_size) {
    int success = 1;
    addrs_t firstBasePtr;
    printf("Running testDoubleInit\n");
    VInit (1000);
    heapDump();
    firstBasePtr = baseptr;
    
    VInit(2000);
    if (baseptr != firstBasePtr) {
        fprintf(stderr, "TEST FAILED: changed base pointer reference\n");
        success = 0;
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testSingleMalloc(int mem_size) {
    int success = 1;
    metadata *pmetadata;
    addrs_t *addr1;
    
    printf("Running testSingleMalloc\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        printf("ERROR: CANNOT INITIALIZE: basepointer is not null\n");
        success = 0;
    }
    
    addr1 = VMalloc (100);
    pmetadata = GET_METADATA(addr1);
    if (pmetadata -> size != 104) {
        fprintf(stderr, "ERROR: malloc incorrect data");
        success = 0;
    }
    heapDump();
    
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testSingleMallocAndFree(int mem_size) {
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1;
    char data1[80];
    
    printf("Running testSingleMallocAndFree\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        printf("CANNOT INITIALIZE: basepointer is not null");
        success = 0;
    }
    
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    heapDump();
    
    if (addr1) {
      VGet ((any_t)data1, addr1, n+1);
      heapDump();
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int test3MallocsFreeLeftFreeMiddle(int mem_size) {
    metadata *pmetadata;
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1, *addr2, *addr3;
    char data1[80];
    char data2[80];
    
    printf("Running test3MallocsFreeLeftFreeMiddle\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        printf("CANNOT INITIALIZE: basepointer is not null");
    }
    
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    pmetadata = GET_METADATA(addr1);
    addr2 = VPut (s, n+1);
    addr3 = VPut (s, n+1);
    heapDump();
    
    if (addr1) {
      VGet ((any_t)data1, addr1, n+1);
      heapDump();
    }
    if (addr2) {
      VGet ((any_t)data2, addr2, n+1);
      heapDump();
    }
    if (pmetadata -> size != 40) {
        fprintf(stderr, "ERROR: size block not correct or memory not free");
        success = 0;
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int test3MallocsFreeRightFreeMiddle(int mem_size) {
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1, *addr2, *addr3;
    char data2[80];
    char data3[80];
    
    printf("Running test3MallocsFreeRightFreeMiddle\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        printf("CANNOT INITIALIZE: basepointer is not null");
    }
    
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    addr2 = VPut (s, n+1);
    addr3 = VPut (s, n+1);
    heapDump();
    
    if (addr3) {
      VGet ((any_t)data3, addr3, n+1);
      heapDump();
    }
    if (addr2) {
      VGet ((any_t)data2, addr2, n+1);
      heapDump();
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int test3MallocsFreeRightLeftFreeMiddle(int mem_size) {
    metadata *pmetadata;
    metadata *pFreeMetadata;
    int success = 1;
    int i, n;
    int n1, n2, n3;
    i = 0;
    //char s[80];
    char s[320];
    addrs_t *addr1, *addr2, *addr3;
    char data1[80];
    //char data2[80];
    //char data3[80];
    char data2[160];
    char data3[320];
    
    printf("Running test3MallocsFreeRightFreeLeftFreeMiddle\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        memset(baseptr + sizeof(metadata), 0, memsize - sizeof(metadata));
        pmetadata = (metadata *) baseptr;
        heapDump();
    } else {
        printf("CANNOT INITIALIZE: basepointer is not null");
        return 0;
    }
    pFreeMetadata = FREE_METADATA;
    //n = sprintf (s, "String 1, the current count is %d\n", i);
    n1 = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n1 + 1);
    n2 = sprintf (s, "String 2, the current count is %d String 2, the current count is\n", i);
    addr2 = VPut (s, n2 + 1);
     n3 = sprintf (s, "String 3, the current count is %d String 3, the current count is String 3, the current count is String 3, the current count is\n", i);
    addr3 = VPut (s, n3 + 1);
    heapDump();
    
    if (addr3) {
      VGet ((any_t)data3, addr3, n3+1);
      heapDump();
    }
    if (addr1) {
      VGet ((any_t)data1, addr1, n1+1);
      heapDump();
    }
    if (addr2) {
      VGet ((any_t)data2, addr2, n2+1);
      heapDump();
    }
    if (pFreeMetadata -> size != 1048560) {
        fprintf(stderr, "ERROR: memory not freed\n");
        success = 0;
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int test2MallocsFreeLeftMallocFitLeft(int mem_size) {
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1, *addr2;
    char data1[80];
    
    printf("Running test2MallocsFreeLeftMallocFitLeft\n");
    VInit (mem_size);
    heapDump();
    
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    addr2 = VPut (s, n+1);
    heapDump();
    
    if (addr1) {
      VGet ((any_t)data1, addr1, n+1);
      heapDump();
    }
    
    addr1 = VPut(s, n+1);
    heapDump();
    
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testFunctionsWithNoInit() {
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1;
    
    printf("Running testFunctionsWithNoInit\n");
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

//*******Prints out an error and does not  initialize**************
int testInitWithHugeMemory() {
    int success = 1;
    printf("Running testInitWithHugeMemory\n");
    VInit (LONG_MAX);
    heapDump();
    
    if (baseptr != NULL) {
        fprintf(stderr, "TEST FAILED");
        success = 0;
    }
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testRoundTo8Byte() {
    int success = 1;
    printf("Running testRoundTo8Byte\n");
    size_t zero, one, two, three, four, five;
    zero = roundTo8Byte(0);
    printf("zero: should be 0, returns %zu\n", zero);
    one = roundTo8Byte(1);
    printf("one: should be 8, returns %zu\n", one);
    two = roundTo8Byte(8);
    printf("two: should be 8, returns %zu\n", two);
    three = roundTo8Byte(9);
    printf("three: should be 16, returns %zu\n", three);
    four = roundTo8Byte(18);
    printf("four: should be 24, returns %zu\n", four);
    five = roundTo8Byte(27);
    printf("five: should be 32, returns %zu\n", five);
    printf("test done\n====================\n\n");
    return success;
}

int testMallocZeroMemory(int mem_size) {
    int success = 1;
    addrs_t *addr1;
    
    printf("Running testMallocZeroMemory\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        fprintf(stderr, "ERROR: CANNOT INITIALIZE: basepointer is not null\n");
        success = 0;
    }
    
    addr1 = VMalloc(0);
    if (addr1 != NULL) {
        fprintf(stderr, "ERROR: not supposed to malloc any space\n");
        success = 0;
    }
    heapDump();
    
    releaseMemory();
    heapDump();
    printf("test done\n====================\n\n");
    return success;
}

int testFreeBadAddress(int mem_size) {
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1, *addr2;
    char data2[80];
    
    printf("Running testFreeBadAddress\n");
    if (baseptr == NULL) {
        VInit (mem_size);
        heapDump();
    } else {
        fprintf(stderr, "CANNOT INITIALIZE: basepointer is not null");
    }
    
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    addr2 = 1;
    if (addr2) {
      VGet ((any_t)data2, addr2, n+1);
      heapDump();
    }
    releaseMemory();
    printf("test done\n====================\n\n");
    return success;
}

int testInitSmallMemory() {
    int success = 1;
    
    printf("Running testInitSmallMemory\n");
    VInit(10);
    if (baseptr != NULL) {
        success = 0;
        fprintf(stderr, "ERROR: baseptr should be NULL\n");
    }
    printHeapStats();
    releaseMemory();
    printf("test done\n====================\n\n");
    return success;
}

int testMallocInsufficientMemory() {
    int success = 1;
    addrs_t *addr;
    
    printf("Running testMallocInsufficientMemory\n");
    VInit(100);
    addr = VMalloc(200);
    if (addr != NULL) {
        success = 0;
        fprintf(stderr, "ERROR: addr should be NULL\n");
    }
    printHeapStats();
    releaseMemory();
    printf("test done\n====================\n\n");
    return success;
}

int testBlockDoesNotFit() {
    metadata *pmetadata;
    int success = 1;
    int i, n;
    i = 0;
    char s[80];
    addrs_t *addr1, *addr2, *addr3;
    
    printf("Running testBlockDoesNotFit\n");
    if (baseptr == NULL) {
        VInit (148);
        pmetadata = (metadata *) baseptr;
        heapDump();
    } else {
        printf("CANNOT INITIALIZE: basepointer is not null\n");
        return 0;
    }
    n = sprintf (s, "String 1, the current count is %d\n", i);
    addr1 = VPut (s, n+1);
    addr2 = VPut (s, n+1);
    // this should not fit
    addr3 = VPut (s, n+1);
    if (addr3 != NULL) {
        fprintf(stderr,"ERROR: block should not fit\n");
        success = 0;
    }
    heapDump();
    releaseMemory();
    printf("test done\n====================\n\n");
    return success;
}

int testGetInvalidSize(int mem_size) {
    int success = 1;
    char s[80];
    char data[80];
    int i = 0;
    int n;
    addrs_t *addr1;
    
    printf("Running testGetInvalidSize\n");
    VInit (mem_size);
    n = sprintf (s, "String 1, the current count %d\n", i);
    addr1 = VPut (s, n+1);
    memset(data, 0, 80);
    VGet((any_t)data, addr1, n + 100);
    if (data[0] != 0) {
        fprintf(stderr, "ERROR: get should have done nothing\n");
        success = 0;
    }
    printf("test done\n====================\n\n");
    return success;
}

void printTestResult(int success) {
    printf("^^^test %s^^^\n\n", success == 0 ? "FAILED" : "succeeded");
}

// this is the main from the assignment
void main(int argc, char **argv) {
    int testSuccess = 1;
    int overallSuccess = 1;
    int mem_size = DEFAULT_MEM_SIZE; // Set DEFAULT_MEM_SIZE to 1<<20 bytes for a heap region
    if  (argc > 2) {
        fprintf (stderr, "Usage: %s [memory area size in bytes]\n", argv[0]);
        exit (1);
    }
    else if (argc == 2)
        mem_size = atoi (argv[1]);

    testSuccess = testFromAssignment(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;
    
    testSuccess = testInit(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;
    
    testSuccess = testDoubleInit(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testSingleMalloc(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testSingleMallocAndFree(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = test3MallocsFreeLeftFreeMiddle(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = test3MallocsFreeRightFreeMiddle(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = test3MallocsFreeRightLeftFreeMiddle(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = test2MallocsFreeLeftMallocFitLeft(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testFunctionsWithNoInit();
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testInitWithHugeMemory();
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testRoundTo8Byte();
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testMallocZeroMemory(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testMallocInsufficientMemory();
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;

    testSuccess = testFreeBadAddress(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;
    
    testSuccess = testInitSmallMemory();
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;
    
    testSuccess = testBlockDoesNotFit();
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;
    
    testSuccess = testGetInvalidSize(mem_size);
    printTestResult(testSuccess);
    overallSuccess &= testSuccess;
    
    if (overallSuccess == 0) {
        printf("\nERROR: test failed\n");
    } else {
        printf("\nAll tests succeed\n");
    }
}





