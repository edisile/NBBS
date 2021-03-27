#ifndef TRANSACTION_MACROS
	#define TRANSACTION_MACROS

	#include <immintrin.h>

	#define __EXEC_BLOCK(...) do {					\
		/* Copy every statement given as input */	\
		__VA_ARGS__;								\
	} while (0)

	#define IN_TRANSACTION _xtest()

	#define TRANSACTION_OK(s) (s == _XBEGIN_STARTED)
	#define TRANSACTION_ABORTED(s) (s & 0x1)
	#define TRANSACTION_RETRY(s) (s & (0x1 << 1))
	#define TRANSACTION_CONFLICT(s) (s & (0x1 << 2))
	#define TRANSACTION_OVERFLOW(s) (s & (0x1 << 3))
	#define TRANSACTION_DEGUB_POINT(s) (s & (0x1 << 4))
	#define TRANSACTION_NESTED(s) (s & (0x1 << 5))

	#define ABORT_CODE(s) (s >> 24) // The upper 8 bits are the abort code

	#define TRY_TRANSACTION(...) ({							\
		unsigned __status = _xbegin();						\
		if (TRANSACTION_OK(__status)) {						\
			__EXEC_BLOCK(__VA_ARGS__);						\
			_xend();										\
		}													\
		/* Just "return" whether the operation went ok, */	\
		/* fallback responsibility is upon the caller! */	\
		__status;											\
	})
#endif