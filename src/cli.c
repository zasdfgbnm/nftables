/*
 * Asynchronous readline-based interactive interface
 *
 * Actually not asynchronous so far, but intended to be.
 *
 * Copyright (c) 2008 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <nftables.h>
#include <parser.h>
#include <erec.h>
#include <utils.h>
#include <iface.h>
#include <cli.h>

#include <libmnl/libmnl.h>

#define CMDLINE_HISTFILE	".nft.history"

static const struct input_descriptor indesc_cli = {
	.type	= INDESC_CLI,
	.name   = "<cli>",
};

static struct parser_state *state;
static struct nft_ctx *cli_nft;
static struct mnl_socket *cli_nf_sock;
static void *scanner;
static char histfile[PATH_MAX];
static char *multiline;
static bool eof;

static char *cli_append_multiline(char *line)
{
	bool complete = false;
	size_t len;
	char *s;

	if (line == NULL && multiline == NULL) {
		eof = true;
		return NULL;
	}

	len = strlen(line);

	if (len == 0)
		return NULL;

	if (line[len - 1] == '\\') {
		line[len - 1] = '\0';
		len--;
	} else if (multiline == NULL)
		return line;
	else
		complete = 1;

	if (multiline == NULL) {
		multiline = line;
		rl_save_prompt();
		rl_clear_message();
		rl_set_prompt(".... ");
	} else {
		len += strlen(multiline);
		s = xmalloc(len + 1);
		snprintf(s, len + 1, "%s%s", multiline, line);
		xfree(multiline);
		multiline = s;
	}
	line = NULL;

	if (complete) {
		line = multiline;
		multiline = NULL;
		rl_restore_prompt();
	}
	return line;
}

static void cli_complete(char *line)
{
	const HIST_ENTRY *hist;
	const char *c;
	LIST_HEAD(msgs);
	int len;
	char *s;

	if (line == NULL) {
		printf("\n");
		cli_exit();
		exit(0);
	}

	line = cli_append_multiline(line);
	if (line == NULL)
		return;

	for (c = line; *c != '\0'; c++)
		if (!isspace(*c))
			break;
	if (*c == '\0')
		return;

	if (!strcmp(line, "quit")) {
		cli_exit();
		exit(0);
	}

	/* avoid duplicate history entries */
	hist = history_get(history_length);
	if (hist == NULL || strcmp(hist->line, line))
		add_history(line);

	len = strlen(line);
	s = xmalloc(len + 2);
	snprintf(s, len + 2, "%s\n", line);
	xfree(line);
	line = s;

	parser_init(cli_nf_sock, &cli_nft->cache, state, &msgs,
		    cli_nft->debug_mask);
	scanner_push_buffer(scanner, &indesc_cli, line);
	nft_run(cli_nft, cli_nf_sock, scanner, state, &msgs);
	erec_print_list(stdout, &msgs, cli_nft->debug_mask);
	xfree(line);
	cache_release(&cli_nft->cache);
	iface_cache_release();
}

static char **cli_completion(const char *text, int start, int end)
{
	return NULL;
}

int cli_init(struct nft_ctx *nft, struct mnl_socket *nf_sock,
	     struct parser_state *_state)
{
	const char *home;

	cli_nf_sock = nf_sock;
	cli_nft = nft;
	rl_readline_name = "nft";
	rl_instream  = stdin;
	rl_outstream = stdout;

	rl_callback_handler_install("nft> ", cli_complete);
	rl_attempted_completion_function = cli_completion;

	home = getenv("HOME");
	if (home == NULL)
		home = "";
	snprintf(histfile, sizeof(histfile), "%s/%s", home, CMDLINE_HISTFILE);

	read_history(histfile);
	history_set_pos(history_length);

	state	= _state;
	scanner = scanner_init(state);

	while (!eof)
		rl_callback_read_char();
	return 0;
}

void cli_exit(void)
{
	rl_callback_handler_remove();
	rl_deprep_terminal();
	write_history(histfile);
}
