#ifndef GCC_WRAPPERS
	//More conveniently named wrappers for GCC built-ins
	#define GCC_WRAPPERS
	
	#define bCAS(ptr, old, new) (__sync_bool_compare_and_swap(ptr, old, new))
	#define mfence __sync_synchronize
	#define atomic_inc(ptr) (__sync_fetch_and_add(ptr, 1))
	#define atomic_dec(ptr) (__sync_fetch_and_sub(ptr, 1))
	#define atomic_add(ptr, val) (__sync_fetch_and_add(ptr, val))
	#define atomic_sub(ptr, val) (__sync_fetch_and_sub(ptr, val))
#endif