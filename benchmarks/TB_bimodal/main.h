#if KERNEL_BD == 0
#define ALLOC_GET_PAR(x,y) x
#define  FREE_GET_PAR(x,y) x
#else
#define ALLOC_GET_PAR(x,y) y
#define  FREE_GET_PAR(x,y) x,y
#endif



void* bd_xx_malloc(size_t);
void  bd_xx_free(FREE_GET_PAR(void*,size_t));

#include "parameters.h"

void bimodal(ALLOC_GET_PAR(unsigned long long fixed_size, unsigned int fixed_order), 
		ALLOC_GET_PAR(unsigned long long max_size, unsigned int max_order), 
		unsigned long long *allocs, unsigned long long *failures, unsigned long long *frees){
	unsigned int j,i, tentativi = BM_ALLOCATIONS;
	unsigned int iterations = BM_ITERATIONS;
#if KERNEL_BD == 0
	void *obt, *cmp = NULL;
	void **addrs = malloc(sizeof(void*)*tentativi);
#else
	unsigned long long cmp = 0ULL;
	unsigned long long *addrs = vmalloc(sizeof(void*)*tentativi);
#endif
	
	unsigned long lfrees = 0;
	unsigned long lfailures = 0;
	unsigned long lallocs = 0;
	
	i = 0;
	j=0;
	for(j=0;j<iterations;j++){
		for(i=0;i<tentativi;i++){
			if (i % 16 == 0) {
				addrs[i] = TO_BE_REPLACED_MALLOC(ALLOC_GET_PAR(max_size, max_order));
			} else {
				addrs[i] = TO_BE_REPLACED_MALLOC(ALLOC_GET_PAR(fixed_size, fixed_order));
			}

			if (addrs[i] == cmp){
				lfailures++;
			}
			else
				lallocs++;
		}
		
		for(i=0;i<tentativi;i++){
			if(addrs[i] != cmp){
				TO_BE_REPLACED_FREE(FREE_GET_PAR(addrs[i], (i % 16 == 0 ? max_order : fixed_order)));
				lfrees++;
			}
		}
	}

	*frees = lfrees;
	*failures = lfailures;
	*allocs = lallocs;

#if KERNEL_BD == 0
	free(addrs);
#else
	vfree(addrs);
#endif
}
