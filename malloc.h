#ifndef MALLOC_H
#define MALLOC_H

int init_malloc(char* mem_block_algo);
void* tmalloc(int size);
void tfree(void* ptr);

#endif