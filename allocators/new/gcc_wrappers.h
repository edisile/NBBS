#ifndef GCC_WRAPPERS
	//More conveniently named wrappers for GCC built-ins
	#define GCC_WRAPPERS
	
	#define bCAS(ptr, old, new) (__sync_bool_compare_and_swap(ptr, old, new))
	#define mfence __sync_synchronize
#endif