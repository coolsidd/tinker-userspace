#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "llist.h"
#include "malloc.h"

#define INCREMENT 4096 // generally 4KB is the default page size

// global variables
LL_HEAD(head);
void *(*mem_block_algo)(int); // function pointer to chosen algorithm
void *(*failsafe_block_algo)(
    int); // failsafe algo is called after requesting memory
void (*defrag_block_algo)(void *); // combine small blocks of memory together
int ret;

// block header structure
/**
 * The block_hdr struct represents the structure of the block header.
 *
 * A block header stores additional information about the newly allocated block.
 * Part of this information helps to keep the track of the block size so that
 * free and realloc can work properly.
 * @sa https://stackoverflow.com/questions/7717296/malloc-header-contents
 */
typedef struct block_hdr {
  struct ll_head *prev;
  struct ll_head *next;
  int size;
  bool block_start;
  bool allocated;
} block_hdr;
typedef block_hdr *Block_hdr;

/*****************************************
 * Block selecting algorithms
 * first_fit_block_algo
 * failsafe_check_new_block
 *****************************************/

/**
 * Function first_fit_block_algo returns unallocated block of the size.
 *
 * Traverse all blocks and return first unallocated block that satisfies size
 * requirement. It takes care to allocate extra space needed for the block
 * header. Returns NULL on failing
 *
 * @param[in] size The amount of size in bytes that is needed (Excluding the
 * size of the header)
 */
void *first_fit_block_algo(int size) {

  // initialize variables to traverse blocks
  void *alloc_block = NULL;
  struct llhead *traverse;
  Block_hdr hdr;

  LL_FOREACH(&head, traverse) {
    hdr = (Block_hdr)traverse;

    // Note: {experiment with this check to get different results
    // checks if block has size + bytes required for next block header}
    if (!hdr->allocated && hdr->size >= size + sizeof(block_hdr)) {
      alloc_block = hdr;
      break;
    }
  }

  return alloc_block;
}

/**
 * Checks if last block satisfied memory requirement
 *
 * Note: Ideally should be called after requesting more memory
 * as newly created arena will be added to tail of blocks
 * Returns header of the tail block if the tail block has space and is
 * unallocated, else returns NULL
 */
void *failsafe_check_tail_block(int size) {

  struct llhead *tail = (&head)->prev;
  Block_hdr hdr = (Block_hdr)tail;

  // return NULL if tail block is same as head
  if (tail == &head)
    return NULL;
  // tail block has is unallocated has space and
  if (!hdr->allocated && hdr->size >= size + sizeof(block_hdr)) {
    return hdr;
  }

  return NULL;
}

/**
 * Merge adjacent unallocated blocks
 *
 * Merges all the adjacent unused "freed" memory. This way larger blocks of
 * memory are now available.
 */
void merge_adjacent_blocks_algo(void *ptr) {

  struct llhead *next, *traverse, *block = (struct llhead *)ptr;
  Block_hdr tmp, hdr = (Block_hdr)ptr;

  // forward pass uptil allocated block or start of next block
  LL_FOREACH_SAFE(block, next, traverse) {

    tmp = (Block_hdr)next;
    if (tmp == ((Block_hdr)&head))
      break;
    if (tmp->allocated || tmp->block_start)
      break;

    hdr->size += tmp->size + sizeof(block_hdr);
    LL_DEL(next);
    printf("merged\n");
  }
}

/**
 * Request more heap memory by incrementing program break
 *
 * Program break represents the first location after the end of the
 * uninitialized data segment. sbrk is a system call which increases/decreases
 * the amount of allocated memory
 * @sa https://en.wikipedia.org/wiki/Sbrk
 * @sa
 * {https://stackoverflow.com/questions/6988487/what-does-the-brk-system-call-do}
 */
int request_memory() {

  // increment current program break location
  void *curr_brk = sbrk(0);
  int ret = brk(curr_brk + INCREMENT);
  if (ret == -1)
    return -1; // failed to find program break location

  // allocate new arena of memory
  Block_hdr hdr = (Block_hdr)curr_brk;
  hdr->size = INCREMENT - sizeof(block_hdr);
  hdr->allocated = false;
  hdr->block_start = true;
  LL_TAIL(&head, ((struct llhead *)hdr));

  return 0; // successfully allocated memory
}

/**
 * Allocate memory from given block. Mark block
 * as allocated and break it into a new unallocated block
 */
void *malloc_mem(void *block, int size) {

  // parse existing block
  void *empty_mem = block + sizeof(block_hdr);
  Block_hdr hdr = (Block_hdr)block;
  int empty_size = hdr->size;

  // create new memory block
  void *new_block = empty_mem + size;
  Block_hdr new_hdr = (Block_hdr)new_block;
  new_hdr->allocated = true;
  new_hdr->size = empty_size - size - sizeof(block_hdr);
  new_hdr->next = hdr->next;
  hdr->size = size;
  LL_ADD(&head, (struct llhead *)new_hdr);

  return empty_mem;
}

/***********************************
 * PUBLIC APIS
 ************************************
 * init_malloc
 * malloc
 * free
 ************************************/

/**
 * Intializes global variables and block finding algorithm
 *
 * Note Currently only supports "first_fit_block_algo",
 * if function is passed invalid function name returns with -1
 */
int init_malloc(char *algo_name) {

  // set mem_block_algo memory allocation
  if (strcmp(algo_name, "first_fit_block_algo") == 0) {
    mem_block_algo = first_fit_block_algo;
  } else {
    return -1;
  }

  failsafe_block_algo = failsafe_check_tail_block;
  defrag_block_algo = merge_adjacent_blocks_algo;

  // request new arena
  request_memory();
}

/**
 * Tries to allocate memory of given size
 *
 * returns pointer to memory if successful else
 * returns NULL
 */
void *tmalloc(int size) {

  void *alloc_block;
  alloc_block = (*mem_block_algo)(size);

  // malloc_mem fails, request more memory from program stack
  if (alloc_block == NULL) {
    ret = request_memory();
    if (ret == 0) { // successful block allocation
      alloc_block = (*failsafe_block_algo)(size);
    }
  }

  if (alloc_block != NULL) {
    return malloc_mem(alloc_block, size);
  } else {
    return NULL;
  }
}

/**
 * Frees given block
 * Note: fails silently
 *
 * @param[in] ptr Pointer to the block that needs to be freed
 */
void tfree(void *ptr) {

  Block_hdr ptr_hdr = (Block_hdr)(ptr - sizeof(block_hdr));
  ptr_hdr->allocated = false;
  (*defrag_block_algo)(ptr_hdr);
}

// int main() {
//     // printf("%d\n", getpid());
//     printf("%d\n", getpid());
//     init_malloc("first_fit_block_algo");
//     int count = 0;
//     while (true) {
//         ret = request_memory();
//         if (ret == -1) break;
//         count++;
//     }
//     printf("%d\n", count);
//     getchar();
// }
