
#include "win32_compat.h"

#ifdef _WIN32

/*
Returns the number of trailing 0-bits in x, starting at the least
significant bit position. If x is 0, the result is undefined.
*/
int __builtin_ctzll(unsigned long long x)
{
   if (0 == x)
      return 0;

   int count = 0;
   while (0 == (x & 1)) {
      ++count;
      x >>= 1;
   }
   return count;
}

#endif /* _WIN32 */
