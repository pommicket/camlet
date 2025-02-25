#ifndef UTIL_H_
#define UTIL_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include "ds.h"

static char *va_sprintf(const char *fmt, va_list args) {
	va_list args_copy;
	va_copy(args_copy, args);
	char fakebuf[2] = {0};
	int ret = vsnprintf(fakebuf, 1, fmt, args_copy);
	va_end(args_copy);
	
	if (ret < 0) return NULL; // bad format or something
	size_t n = (size_t)ret;
	char *str = calloc(1, n + 1);
	vsnprintf(str, n + 1, fmt, args);
	return str;
}

static char *a_sprintf(PRINTF_FORMAT_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
static char *a_sprintf(const char *fmt, ...) {
	// idk if you can always just pass NULL to vsnprintf
	va_list args;
	va_start(args, fmt);
	char *str = va_sprintf(fmt, args);
	va_end(args);
	return str;
}


static double get_time_double(void) {
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint8_t popcount64(uint64_t x) {
	uint8_t cnt = 0;
	while (x) {
		x &= x - 1;
		cnt++;
	}
	return cnt;
}

#endif
