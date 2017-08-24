/*
 * Copyright (c) 2008 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 */

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/types.h>

#include <nftables.h>
#include <utils.h>
#include <parser.h>
#include <rule.h>
#include <netlink.h>
#include <erec.h>
#include <mnl.h>
#include <iface.h>
#include <cli.h>

static struct nft_ctx nft;

enum opt_vals {
	OPT_HELP		= 'h',
	OPT_VERSION		= 'v',
	OPT_CHECK		= 'c',
	OPT_FILE		= 'f',
	OPT_INTERACTIVE		= 'i',
	OPT_INCLUDEPATH		= 'I',
	OPT_NUMERIC		= 'n',
	OPT_STATELESS		= 's',
	OPT_IP2NAME		= 'N',
	OPT_DEBUG		= 'd',
	OPT_HANDLE_OUTPUT	= 'a',
	OPT_ECHO		= 'e',
	OPT_INVALID		= '?',
};

#define OPTSTRING	"hvcf:iI:vnsNae"

static const struct option options[] = {
	{
		.name		= "help",
		.val		= OPT_HELP,
	},
	{
		.name		= "version",
		.val		= OPT_VERSION,
	},
	{
		.name		= "check",
		.val		= OPT_CHECK,
	},
	{
		.name		= "file",
		.val		= OPT_FILE,
		.has_arg	= 1,
	},
	{
		.name		= "interactive",
		.val		= OPT_INTERACTIVE,
	},
	{
		.name		= "numeric",
		.val		= OPT_NUMERIC,
	},
	{
		.name		= "stateless",
		.val		= OPT_STATELESS,
	},
	{
		.name		= "reversedns",
		.val		= OPT_IP2NAME,
	},
	{
		.name		= "includepath",
		.val		= OPT_INCLUDEPATH,
		.has_arg	= 1,
	},
	{
		.name		= "debug",
		.val		= OPT_DEBUG,
		.has_arg	= 1,
	},
	{
		.name		= "handle",
		.val		= OPT_HANDLE_OUTPUT,
	},
	{
		.name		= "echo",
		.val		= OPT_ECHO,
	},
	{
		.name		= NULL
	}
};

static void show_help(const char *name)
{
	printf(
"Usage: %s [ options ] [ cmds... ]\n"
"\n"
"Options:\n"
"  -h, --help			Show this help\n"
"  -v, --version			Show version information\n"
"\n"
"  -c, --check			Check commands validity without actually applying the changes.\n"
"  -f, --file <filename>		Read input from <filename>\n"
"  -i, --interactive		Read input from interactive CLI\n"
"\n"
"  -n, --numeric			When specified once, show network addresses numerically (default behaviour).\n"
"  				Specify twice to also show Internet services (port numbers) numerically.\n"
"				Specify three times to also show protocols, user IDs, and group IDs numerically.\n"
"  -s, --stateless		Omit stateful information of ruleset.\n"
"  -N				Translate IP addresses to names.\n"
"  -a, --handle			Output rule handle.\n"
"  -e, --echo			Echo what has been added, inserted or replaced.\n"
"  -I, --includepath <directory>	Add <directory> to the paths searched for include files. Default is: %s\n"
"  --debug <level [,level...]>	Specify debugging level (scanner, parser, eval, netlink, mnl, proto-ctx, segtree, all)\n"
"\n",
	name, DEFAULT_INCLUDE_PATH);
}

static const struct {
	const char		*name;
	enum debug_level	level;
} debug_param[] = {
	{
		.name		= "scanner",
		.level		= DEBUG_SCANNER,
	},
	{
		.name		= "parser",
		.level		= DEBUG_PARSER,
	},
	{
		.name		= "eval",
		.level		= DEBUG_EVALUATION,
	},
	{
		.name		= "netlink",
		.level		= DEBUG_NETLINK,
	},
	{
		.name		= "mnl",
		.level		= DEBUG_MNL,
	},
	{
		.name		= "proto-ctx",
		.level		= DEBUG_PROTO_CTX,
	},
	{
		.name		= "segtree",
		.level		= DEBUG_SEGTREE,
	},
	{
		.name		= "all",
		.level		= ~0,
	},
};

static const struct input_descriptor indesc_cmdline = {
	.type	= INDESC_BUFFER,
	.name	= "<cmdline>",
};

static int nft_netlink(struct nft_ctx *nft,
		       struct parser_state *state, struct list_head *msgs,
		       struct mnl_socket *nf_sock)
{
	uint32_t batch_seqnum, seqnum = 0;
	struct nftnl_batch *batch;
	struct netlink_ctx ctx;
	struct cmd *cmd;
	struct mnl_err *err, *tmp;
	LIST_HEAD(err_list);
	bool batch_supported = netlink_batch_supported(nf_sock, &seqnum);
	int ret = 0;

	batch = mnl_batch_init();

	batch_seqnum = mnl_batch_begin(batch, mnl_seqnum_alloc(&seqnum));
	list_for_each_entry(cmd, &state->cmds, list) {
		memset(&ctx, 0, sizeof(ctx));
		ctx.msgs = msgs;
		ctx.seqnum = cmd->seqnum = mnl_seqnum_alloc(&seqnum);
		ctx.batch = batch;
		ctx.batch_supported = batch_supported;
		ctx.octx = &nft->output;
		ctx.nf_sock = nf_sock;
		ctx.cache = &nft->cache;
		ctx.debug_mask = nft->debug_mask;
		init_list_head(&ctx.list);
		ret = do_command(&ctx, cmd);
		if (ret < 0)
			goto out;
	}
	if (!nft->check)
		mnl_batch_end(batch, mnl_seqnum_alloc(&seqnum));

	if (!mnl_batch_ready(batch))
		goto out;

	ret = netlink_batch_send(&ctx, &err_list);

	list_for_each_entry_safe(err, tmp, &err_list, head) {
		list_for_each_entry(cmd, &state->cmds, list) {
			if (err->seqnum == cmd->seqnum ||
			    err->seqnum == batch_seqnum) {
				netlink_io_error(&ctx, &cmd->location,
						 "Could not process rule: %s",
						 strerror(err->err));
				errno = err->err;
				if (err->seqnum == cmd->seqnum) {
					mnl_err_list_free(err);
					break;
				}
			}
		}
	}
out:
	mnl_batch_reset(batch);
	return ret;
}

int nft_run(struct nft_ctx *nft, struct mnl_socket *nf_sock,
	    void *scanner, struct parser_state *state,
	    struct list_head *msgs)
{
	struct cmd *cmd, *next;
	int ret;

	ret = nft_parse(nft, scanner, state);
	if (ret != 0 || state->nerrs > 0) {
		ret = -1;
		goto err1;
	}

	list_for_each_entry(cmd, &state->cmds, list)
		nft_cmd_expand(cmd);

	ret = nft_netlink(nft, state, msgs, nf_sock);
err1:
	list_for_each_entry_safe(cmd, next, &state->cmds, list) {
		list_del(&cmd->list);
		cmd_free(cmd);
	}

	return ret;
}

void nft_init(void)
{
	mark_table_init();
	realm_table_rt_init();
	devgroup_table_init();
	realm_table_meta_init();
	ct_label_table_init();
	gmp_init();
#ifdef HAVE_LIBXTABLES
	xt_init();
#endif
}

void nft_exit(void)
{
	ct_label_table_exit();
	realm_table_rt_exit();
	devgroup_table_exit();
	realm_table_meta_exit();
	mark_table_exit();
}

static void nft_ctx_init(struct nft_ctx *nft)
{
	nft->include_paths[0]	= DEFAULT_INCLUDE_PATH;
	nft->num_include_paths	= 1;
	nft->parser_max_errors = 10;
}

int main(int argc, char * const *argv)
{
	struct parser_state state;
	void *scanner;
	LIST_HEAD(msgs);
	char *buf = NULL, *filename = NULL;
	unsigned int len;
	bool interactive = false;
	int i, val, rc = NFT_EXIT_SUCCESS;
	struct mnl_socket *nf_sock;

	init_list_head(&nft.cache.list);

	nft_init();
	nft_ctx_init(&nft);

	nf_sock = netlink_open_sock();
	while (1) {
		val = getopt_long(argc, argv, OPTSTRING, options, NULL);
		if (val == -1)
			break;

		switch (val) {
		case OPT_HELP:
			show_help(argv[0]);
			exit(NFT_EXIT_SUCCESS);
		case OPT_VERSION:
			printf("%s v%s (%s)\n",
			       PACKAGE_NAME, PACKAGE_VERSION, RELEASE_NAME);
			exit(NFT_EXIT_SUCCESS);
		case OPT_CHECK:
			nft.check = true;
			break;
		case OPT_FILE:
			filename = optarg;
			break;
		case OPT_INTERACTIVE:
			interactive = true;
			break;
		case OPT_INCLUDEPATH:
			if (nft.num_include_paths >= INCLUDE_PATHS_MAX) {
				fprintf(stderr, "Too many include paths "
						"specified, max. %u\n",
					INCLUDE_PATHS_MAX - 1);
				exit(NFT_EXIT_FAILURE);
			}
			nft.include_paths[nft.num_include_paths++] = optarg;
			break;
		case OPT_NUMERIC:
			if (++nft.output.numeric > NUMERIC_ALL) {
				fprintf(stderr, "Too many numeric options "
						"used, max. %u\n",
					NUMERIC_ALL);
				exit(NFT_EXIT_FAILURE);
			}
			break;
		case OPT_STATELESS:
			nft.output.stateless++;
			break;
		case OPT_IP2NAME:
			nft.output.ip2name++;
			break;
		case OPT_DEBUG:
			for (;;) {
				unsigned int i;
				char *end;

				end = strchr(optarg, ',');
				if (end)
					*end = '\0';

				for (i = 0; i < array_size(debug_param); i++) {
					if (strcmp(debug_param[i].name, optarg))
						continue;
					nft.debug_mask |= debug_param[i].level;
					break;
				}

				if (i == array_size(debug_param)) {
					fprintf(stderr, "invalid debug parameter `%s'\n",
						optarg);
					exit(NFT_EXIT_FAILURE);
				}

				if (end == NULL)
					break;
				optarg = end + 1;
			}
			break;
		case OPT_HANDLE_OUTPUT:
			nft.output.handle++;
			break;
		case OPT_ECHO:
			nft.output.echo++;
			break;
		case OPT_INVALID:
			exit(NFT_EXIT_FAILURE);
		}
	}

	if (optind != argc) {
		for (len = 0, i = optind; i < argc; i++)
			len += strlen(argv[i]) + strlen(" ");

		buf = xzalloc(len + 2);
		for (i = optind; i < argc; i++) {
			strcat(buf, argv[i]);
			if (i + 1 < argc)
				strcat(buf, " ");
		}
		strcat(buf, "\n");
		parser_init(nf_sock, &nft.cache, &state, &msgs, nft.debug_mask);
		scanner = scanner_init(&state);
		scanner_push_buffer(scanner, &indesc_cmdline, buf);
	} else if (filename != NULL) {
		rc = cache_update(nf_sock, &nft.cache, CMD_INVALID, &msgs,
				  nft.debug_mask);
		if (rc < 0)
			return rc;

		parser_init(nf_sock, &nft.cache, &state, &msgs, nft.debug_mask);
		scanner = scanner_init(&state);
		if (scanner_read_file(scanner, filename, &internal_location) < 0)
			goto out;
	} else if (interactive) {
		if (cli_init(&nft, nf_sock, &state) < 0) {
			fprintf(stderr, "%s: interactive CLI not supported in this build\n",
				argv[0]);
			exit(NFT_EXIT_FAILURE);
		}
		return 0;
	} else {
		fprintf(stderr, "%s: no command specified\n", argv[0]);
		exit(NFT_EXIT_FAILURE);
	}

	if (nft_run(&nft, nf_sock, scanner, &state, &msgs) != 0)
		rc = NFT_EXIT_FAILURE;
out:
	erec_print_list(stderr, &msgs, nft.debug_mask);
	scanner_destroy(scanner);
	xfree(buf);
	cache_release(&nft.cache);
	iface_cache_release();
	netlink_close_sock(nf_sock);
	nft_exit();

	return rc;
}
