/*
 * Copyright (c) International Business Machines Corp., 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * An utility to create UBI volumes.
 *
 * Author: Artem B. Bityutskiy <dedekind@linutronix.de>
 *         Frank Haverkamp <haver@vnet.ibm.com>
 *
 * 1.0 Initial release
 * 1.1 Does not support erase blocks anymore. This is replaced by
 *     the number of bytes.
 * 1.2 Reworked the user-interface to use argp.
 * 1.3 Removed argp because we want to use uClibc.
 * 1.4 Minor cleanups
 * 1.5 Use a different libubi
 * 1.6 Various fixes
 */

#include <stdio.h>
#include <stdint.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <config.h>
#include <libubi.h>
#include "common.h"

#define PROGRAM_VERSION "1.6"
#define PROGRAM_NAME    "ubimkvol"

/* Maximum device node name length */
#define MAX_NODE_LEN 255

/*
 * The variables below	are set by command line arguments.
 */
struct args {
	int devn;
	int vol_id;
	int vol_type;
	long long bytes;
	int alignment;
	char *name;
	int nlen;
	char node[MAX_NODE_LEN + 1];
	int maxavs;
};

static struct args myargs = {
	.vol_type = UBI_DYNAMIC_VOLUME,
	.devn = -1,
	.bytes = 0,
	.alignment = 1,
	.vol_id = UBI_VOL_NUM_AUTO,
	.name = NULL,
	.nlen = 0,
	.maxavs = 0,
};

static int param_sanity_check(struct args *args, libubi_t libubi);

static char doc[] = "Version " PROGRAM_VERSION "\n"
PROGRAM_NAME " - a tool to create UBI volumes.";

static const char *optionsstr =
"-a, --alignment=<alignment>   volume alignment (default is 1)\n"
"-d, --devn=<devn>             UBI device number (depricated, do not use)"
"-n, --vol_id=<volume id>      UBI volume ID, if not specified, the volume ID\n"
"                              will be assigned automatically\n"
"-N, --name=<name>             volume name\n"
"-s, --size=<bytes>            volume size volume size in bytes, kilobytes (KiB)\n"
"                              or megabytes (MiB)\n"
"-m, --maxavsize               set volume size to maximum available size\n"
"-t, --type=<static|dynamic>   volume type (dynamic, static), default is dynamic\n"
"-h, --help                    help message\n"
"-V, --version                 print program version";

static const char *usage =
"Usage: " PROGRAM_NAME " <device name> [-h] [-a <alignment>] [-d <devn>] [-n <volume id>]\n"
"\t\t\t[-N <name>] [-s <bytes>] [-t <static|dynamic>] [-V] [-m]\n"
"\t\t\t[--alignment=<alignment>] [--devn=<devn>] [--vol_id=<volume id>]\n"
"\t\t\t[--name=<name>] [--size=<bytes>] [--type=<static|dynamic>]\n"
"\t\t\t[--help] [--version] [--maxavsize]";

struct option long_options[] = {
	{ .name = "alignment", .has_arg = 1, .flag = NULL, .val = 'a' },
	{ .name = "devn",      .has_arg = 1, .flag = NULL, .val = 'd' },
	{ .name = "vol_id",    .has_arg = 1, .flag = NULL, .val = 'n' },
	{ .name = "name",      .has_arg = 1, .flag = NULL, .val = 'N' },
	{ .name = "size",      .has_arg = 1, .flag = NULL, .val = 's' },
	{ .name = "type",      .has_arg = 1, .flag = NULL, .val = 't' },
	{ .name = "help",      .has_arg = 0, .flag = NULL, .val = 'h' },
	{ .name = "version",   .has_arg = 0, .flag = NULL, .val = 'V' },
	{ .name = "maxavsize", .has_arg = 0, .flag = NULL, .val = 'm' },
	{ NULL, 0, NULL, 0}
};

static int parse_opt(int argc, char * const argv[], struct args *args)
{
	char *endp;

	while (1) {
		int key;

		key = getopt_long(argc - 1, &argv[1], "a:d:n:N:s:t:hVm",
				  long_options, NULL);
		if (key == -1)
			break;

		switch (key) {
		case 't':
			if (!strcmp(optarg, "dynamic"))
				args->vol_type = UBI_DYNAMIC_VOLUME;
			else if (!strcmp(optarg, "static"))
				args->vol_type = UBI_STATIC_VOLUME;
			else {
				errmsg("bad volume type: \"%s\"", optarg);
				return -1;
			}
			break;

		case 's':
			args->bytes = strtoull(optarg, &endp, 0);
			if (endp == optarg || args->bytes < 0) {
				errmsg("bad volume size: \"%s\"", optarg);
				return -1;
			}
			if (*endp != '\0') {
				int mult = ubiutils_get_multiplier(endp);

				if (mult == -1) {
					errmsg("bad size specifier: \"%s\" - "
					       "should be 'KiB', 'MiB' or 'GiB'", endp);
					return -1;
				}
				args->bytes *= mult;
			}
			break;

		case 'a':
			args->alignment = strtoul(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg || args->alignment <= 0) {
				errmsg("bad volume alignment: \"%s\"", optarg);
				return -1;
			}
			break;

		case 'd':
			args->devn = strtoul(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg || args->devn < 0) {
				errmsg("bad UBI device number: \"%s\"", optarg);
				return -1;
			}

			warnmsg("-d and --devn options are depricated and will be removed"
				"soon, pass UBI device node name instead\n"
				"Example: " PROGRAM_NAME " /dev/ubi0, instead of "
				PROGRAM_NAME " -d 0");
			sprintf(args->node, "/dev/ubi%d", args->devn);
			break;

		case 'n':
			args->vol_id = strtoul(optarg, &endp, 0);
			if (*endp != '\0' || endp == optarg || args->vol_id < 0) {
				errmsg("bad volume ID: " "\"%s\"", optarg);
				return -1;
			}
			break;

		case 'N':
			args->name = optarg;
			args->nlen = strlen(args->name);
			break;

		case 'h':
			fprintf(stderr, "%s\n\n", doc);
			fprintf(stderr, "%s\n\n", usage);
			fprintf(stderr, "%s\n", optionsstr);
			exit(0);

		case ':':
			errmsg("parameter is missing");
			return -1;

		case 'V':
			fprintf(stderr, "%s\n", PROGRAM_VERSION);
			exit(0);

		case 'm':
			args->maxavs = 1;
			break;

		default:
			fprintf(stderr, "Use -h for help");
			exit(-1);
		}
	}

	return 0;
}

static int param_sanity_check(struct args *args, libubi_t libubi)
{
	int err, len;
	struct ubi_info ubi;

	if (args->bytes == 0 && !args->maxavs) {
		errmsg("volume size was not specified (use -h for help)");
		return -1;
	}

	if (args->name == NULL) {
		errmsg("volume name was not specified (use -h for help)");
		return -1;
	}

	err = ubi_get_info(libubi, &ubi);
	if (err) {
		errmsg("cannot get UBI information");
		perror("ubi_get_info");
		return -1;
	}

	if (args->devn >= (int)ubi.dev_count) {
		errmsg("UBI device %d does not exist", args->devn);
		return -1;
	}

	len = strlen(args->name);
	if (len > UBI_MAX_VOLUME_NAME) {
		errmsg("too long name (%d symbols), max is %d",
			len, UBI_MAX_VOLUME_NAME);
		return -1;
	}

	return 0;
}

int main(int argc, char * const argv[])
{
	int err;
	libubi_t libubi;
	struct ubi_dev_info dev_info;
	struct ubi_vol_info vol_info;
	struct ubi_mkvol_request req;

	if (argc < 2) {
		errmsg("UBI device name was not specified (use -h for help)");
		return -1;
	}

	if (argc < 3) {
		errmsg("too few arguments (use -h for help)");
		return -1;
	}

	if (strlen(argv[1]) > MAX_NODE_LEN) {
		errmsg("too long device node name: \"%s\" (%d characters), max. is %d",
		       argv[1], strlen(argv[1]), MAX_NODE_LEN);
		return -1;
	}

	strcpy(myargs.node, argv[1]);

	err = parse_opt(argc, (char **)argv, &myargs);
	if (err)
		return err;

	libubi = libubi_open();
	if (libubi == NULL) {
		errmsg("cannot open libubi");
		perror("libubi_open");
		return -1;
	}

	err = param_sanity_check(&myargs, libubi);
	if (err)
		goto out_libubi;

	err = ubi_get_dev_info(libubi, myargs.node, &dev_info);
	if (err) {
		errmsg("cannot get information about UBI device number %d (%s)",
		       myargs.devn, myargs.node);
		perror("ubi_get_dev_info");
		goto out_libubi;
	}

	if (myargs.maxavs) {
		myargs.bytes = dev_info.avail_bytes;
		printf("Set volume size to %lld\n", req.bytes);
	}

	req.vol_id = myargs.vol_id;
	req.alignment = myargs.alignment;
	req.bytes = myargs.bytes;
	req.vol_type = myargs.vol_type;
	req.name = myargs.name;

	err = ubi_mkvol(libubi, myargs.node, &req);
	if (err < 0) {
		errmsg("cannot UBI create volume");
		perror("ubi_mkvol");
		goto out_libubi;
	}

	myargs.vol_id = req.vol_id;

	/* Print information about the created device */
	err = ubi_get_vol_info1(libubi, dev_info.dev_num, myargs.vol_id, &vol_info);
	if (err) {
		errmsg("cannot get information about newly created UBI volume");
		perror("ubi_get_vol_info1");
		goto out_libubi;
	}

	printf("Volume ID is %d, size %lld LEBs (%lld bytes, ",
	       vol_info.vol_id, vol_info.rsvd_bytes / vol_info.eb_size,
	       vol_info.rsvd_bytes);

	if (vol_info.rsvd_bytes > 1024 * 1024 * 1024)
		printf("%.1f GiB)", (double)vol_info.rsvd_bytes / (1024 * 1024 * 1024));
	else if (vol_info.rsvd_bytes > 1024 * 1024)
		printf("%.1f MiB)", (double)vol_info.rsvd_bytes / (1024 * 1024));
	else
		printf("%.1f KiB)", (double)vol_info.rsvd_bytes / 1024);

	printf(" LEB size is %d bytes (%.1f KiB), %s volume, name \"%s\"\n",
	       vol_info.eb_size, ((double) vol_info.eb_size) / 1024,
	       req.vol_type == UBI_DYNAMIC_VOLUME ? "dynamic" : "static",
	       vol_info.name);

	libubi_close(libubi);
	return 0;

out_libubi:
	libubi_close(libubi);
	return -1;
}
