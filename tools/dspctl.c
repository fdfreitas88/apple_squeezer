#include "../squeezelite.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_LIMIT (64 * 1024)

log_level loglevel = lERROR;

const char *logtime(void) { return "dspctl"; }
void logprint(const char *format, ...) { (void)format; }

static void usage(const char *name) {
	fprintf(stderr,
		"Usage:\n"
		"  %s validate FILE [PLAYER_ID]\n"
		"  %s options FILE [PLAYER_ID]\n"
		"  %s save FILE PLAYER_ID       # JSON is read from stdin\n"
		"  %s rollback FILE\n"
		"  %s bypass FILE PLAYER_ID true|false\n"
		"  %s response FILE [PLAYER_ID] [RATE] [POINTS]\n", name, name, name, name, name, name);
}

static char *read_stdin(void) {
	char *data = malloc(INPUT_LIMIT + 1);
	size_t used = 0, count;
	if (!data) return NULL;
	while ((count = fread(data + used, 1, INPUT_LIMIT - used, stdin)) != 0) {
		used += count;
		if (used == INPUT_LIMIT) {
			if (fgetc(stdin) != EOF) { free(data); errno = EFBIG; return NULL; }
			break;
		}
	}
	if (ferror(stdin)) { free(data); return NULL; }
	data[used] = '\0';
	return data;
}

static char *read_file(const char *path) {
	FILE *file = fopen(path, "rb");
	char *data;
	long size;
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || size > INPUT_LIMIT ||
		fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
	data = malloc((size_t)size + 1);
	if (!data) { fclose(file); return NULL; }
	if (fread(data, 1, (size_t)size, file) != (size_t)size) { free(data); fclose(file); return NULL; }
	data[size] = '\0';
	fclose(file);
	return data;
}

static bool set_bypass(const char *path, const char *player, const char *value) {
	char *json = read_file(path), *key, *colon, *begin, *end, *replacement;
	size_t prefix, suffix, value_length = strlen(value);
	bool saved;
	if (!json) return false;
	key = strstr(json, "\"bypass\"");
	colon = key ? strchr(key + 8, ':') : NULL;
	begin = colon ? colon + 1 : NULL;
	while (begin && *begin && strchr(" \t\r\n", *begin)) begin++;
	if (!begin) { free(json); return false; }
	if (!strncmp(begin, "true", 4)) end = begin + 4;
	else if (!strncmp(begin, "false", 5)) end = begin + 5;
	else { free(json); return false; }
	prefix = (size_t)(begin - json); suffix = strlen(end);
	replacement = malloc(prefix + value_length + suffix + 1);
	if (!replacement) { free(json); return false; }
	memcpy(replacement, json, prefix);
	memcpy(replacement + prefix, value, value_length);
	memcpy(replacement + prefix + value_length, end, suffix + 1);
	saved = dsp_config_save(path, player, replacement);
	free(replacement); free(json);
	return saved;
}

int main(int argc, char **argv) {
	char *options, *json;
	if (argc < 3) { usage(argv[0]); return 2; }
	if (!strcmp(argv[1], "validate") || !strcmp(argv[1], "options")) {
		if (argc > 4) { usage(argv[0]); return 2; }
		options = dsp_config_load_options(argv[2], argc == 4 ? argv[3] : NULL);
		if (!options) { fprintf(stderr, "invalid DSP configuration\n"); return 1; }
		if (!strcmp(argv[1], "options")) puts(options);
		free(options);
		return 0;
	}
	if (!strcmp(argv[1], "save")) {
		if (argc != 4) { usage(argv[0]); return 2; }
		json = read_stdin();
		if (!json) { fprintf(stderr, "unable to read DSP configuration\n"); return 1; }
		if (!dsp_config_save(argv[2], argv[3], json)) {
			free(json); fprintf(stderr, "invalid or unwritable DSP configuration\n"); return 1;
		}
		free(json);
		return 0;
	}
	if (!strcmp(argv[1], "rollback")) {
		if (argc != 3) { usage(argv[0]); return 2; }
		if (!dsp_config_rollback(argv[2])) { fprintf(stderr, "no DSP rollback is available\n"); return 1; }
		return 0;
	}
	if (!strcmp(argv[1], "bypass")) {
		if (argc != 5 || (strcmp(argv[4], "true") && strcmp(argv[4], "false"))) {
			usage(argv[0]); return 2;
		}
		if (!set_bypass(argv[2], argv[3], argv[4])) {
			fprintf(stderr, "unable to update DSP bypass\n"); return 1;
		}
		return 0;
	}
	if (!strcmp(argv[1], "response")) {
		unsigned rate = argc >= 5 ? (unsigned)strtoul(argv[4], NULL, 10) : 48000;
		unsigned points = argc >= 6 ? (unsigned)strtoul(argv[5], NULL, 10) : 128;
		char *response;
		if (argc < 3 || argc > 6 || rate < 32000 || rate > 768000 || points < 16 || points > 1024) {
			usage(argv[0]); return 2;
		}
		options = dsp_config_load_options(argv[2], argc >= 4 ? argv[3] : NULL);
		if (!options || !dsp_init(options) || !dsp_newstream(rate)) {
			free(options); fprintf(stderr, "unable to prepare DSP response\n"); return 1;
		}
		free(options);
		response = malloc((size_t)points * 96 + 4);
		if (!response || !dsp_response_json(response, (size_t)points * 96 + 4, points)) {
			free(response); fprintf(stderr, "unable to calculate DSP response\n"); return 1;
		}
		puts(response); free(response); return 0;
	}
	usage(argv[0]);
	return 2;
}
