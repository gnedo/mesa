#ifndef __WIN32_COMPAT_H__
#define __WIN32_COMPAT_H__

#ifdef _WIN32

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
Returns the number of trailing 0-bits in x, starting at the least
significant bit position. If x is 0, the result is undefined.
*/
int __builtin_ctzll(unsigned long long);


#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* __WIN32_COMPAT_H__ */
