#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int argc;
	char **argv;
	const char *options_with_value;

	const char *option;
	const char *value;

	int i;
	unsigned char no_option;
} ArgParser;

static int arg_parser_next(ArgParser *parser) {
top:
	if (parser->i <= 0)
		parser->i = 1;
	if (parser->i >= parser->argc)
		return 0;
	const char *const arg = parser->argv[parser->i++];
	if (parser->no_option)
		goto no_option;
	if (arg[0] == '-') {
		const char *option = arg + 1;
		if (option[0] == '-') {
			option += 1;
		}
		if (option[0] == '\0') {
			parser->no_option = 1;
			goto top;
		}
		int requires_arg = 0;
		const char *p = parser->options_with_value;
		while (p && *p) {
			size_t n = strcspn(p, ",");
			if (strncmp(p, option, n) == 0 && option[n] == 0) {
				requires_arg = 1;
				break;
			}
			p += n;
			while (*p == ',')
				p += 1;
		}
		if (requires_arg) {
			parser->option = option;
			if (parser->i >= parser->argc) {
				fprintf(stderr, "Error: no value provided for option %s\n", option);
				exit(EXIT_FAILURE);
			}
			parser->value = parser->argv[parser->i++];
		} else {
			parser->option = option;
			parser->value = 0;
		}
		return 1;
	}
no_option:
	parser->option = 0;
	parser->value = arg;
	return 1;
}
