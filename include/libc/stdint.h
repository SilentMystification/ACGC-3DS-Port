#ifndef _AC_LIBC_STDINT_H_
#define _AC_LIBC_STDINT_H_

#ifdef TARGET_PC
#include_next <stdint.h>
#else

typedef unsigned long int uintptr_t;

#endif /* TARGET_PC */
#endif
