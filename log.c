#include "log.h"
#include <stdbool.h>
#include <unistd.h>

static FILE *log_file = NULL;

static void get_timestamp(char timestamp[24]) {
	struct tm tm = {0};
	localtime_r((const time_t[1]) { time(NULL) }, &tm);
	strftime(timestamp, 24, "[%Y-%m-%d %H:%M:%S]", &tm);
}

void log_init(const char *out) {
	assert(!log_file);
	log_file = fopen(out, "a");
	if (log_file) {
		char timestamp[24] = {0};
		get_timestamp(timestamp);
		fprintf(log_file, "\n\n--- new session %s ---\n", timestamp);
	} else {
		perror("fopen");
	}
}

void log_message(int severity, const char *fmt, va_list args) {
	char *message = va_sprintf(fmt, args);
	if (!message) return;
	if (log_file) {
		char timestamp[24] = {0};
		get_timestamp(timestamp);
		fprintf(log_file, "%s ", timestamp);
	}
	const char *color_start = "", *severity_str = "???";
	switch (severity) {
	case LOG_ERROR:
		color_start = "\x1b[91m";
		severity_str = "error";
		break;
	case LOG_WARNING:
		color_start = "\x1b[93m";
		severity_str = "warning";
		break;
	}
	bool color = isatty(2) == 1;
	if (color)
		fprintf(stderr, "%s", color_start);
	fprintf(stderr, "%s: ", severity_str);
	if (color)
		fprintf(stderr, "\x1b[0m");
	fprintf(stderr, "%s\n", message);
	if (log_file) fprintf(log_file, "%s: %s\n", severity_str, message);
	free(message);
}

void log_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_message(LOG_ERROR, fmt, args);
	va_end(args);
}

void log_warning(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_message(LOG_WARNING, fmt, args);
	va_end(args);
}
