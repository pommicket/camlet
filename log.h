#ifndef LOG_H_
#define LOG_H_

#include "util.h"

enum {
	LOG_WARNING = 20,
	LOG_ERROR = 30,
};

void log_init(const char *out);
void log_message(int severity, const char *fmt, va_list args);
void log_error(PRINTF_FORMAT_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
void log_warning(PRINTF_FORMAT_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
/// Like `perror`, but outputs to log and allows format string.
void log_perror(PRINTF_FORMAT_STRING const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);

#endif
