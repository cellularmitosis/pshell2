#ifndef _CC_MALLOC_H_
#define _CC_MALLOC_H_

void* cc_malloc(int nbytes, int zero);
void cc_free(void* m);
void cc_free_all(void);

#endif
