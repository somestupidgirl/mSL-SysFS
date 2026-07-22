/*
 *  mount_sysfs.c
 *  mount_sysfs
 *
 * The sysfs-specific mount command, which must be installed in the /sbin
 * directory (or /usr/local/sbin). It supports the standard mount options; sysfs
 * has no filesystem-specific options yet (unlike procfs's procperms), but the
 * option-parsing plumbing is kept so options can be added without reworking this
 * command.
 */

#ifndef __FSBUNDLE__
#define __FSBUNDLE__
#endif

#include <sys/mount.h>

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <fs/sysfs/sysfs.h>

#include <libutil/mntopts.h>

/*
 * Forward declarations of local functions.
 */
static void usage(char *name);

/*
 * Verbose logging flag and logging level for syslog(3)
 */
static const int SYSFS_SYSLOG_LEVEL = LOG_INFO;
static _Bool verbose = FALSE;

/*
 * Mount options.
 */
static struct mntopt mopts[] = {
    MOPT_STDOPTS,
    MOPT_SYSFS,
};

int main(int argc, char *argv[])
{
    /*
     * -- Argument processing. Extracts mount options --
     */
    char *prog_name = basename(argv[0]);

    /*
     * Default generic mount options and sysfs options, which can be overridden
     * using the -o option.
     */
    int generic_options = 0;
    int sysfs_options = 0;

    opterr = 0;  /* Silence default messages from getopt() */
    int option;
    while ((option = getopt(argc, argv, "vo:?h")) != -1) {
        switch (option) {
        case '?':
            /* FALLTHRU */
        case 'h':
            usage(prog_name);
            /* NOTREACHED */
        case 'v':
            verbose = TRUE;
            break;
        case 'o': {
            mntoptparse_t mntops = getmntopts(optarg, mopts, &generic_options, &sysfs_options);
            freemntopts(mntops);
            break;
        }
        default: /* Unrecognized option. */
            usage(prog_name);
            /* NOTREACHED */
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 2) {
        /* Expecting special and mount point arguments. */
        usage(argv[0]);
        /* NOTREACHED */
    }

    /*
     * -- Mount the file system --
     *
     * Pass a page-sized, zeroed buffer as the mount data rather than a bare
     * sfsmount_args_t. The kext only interprets the leading sfsmount_args_t
     * (mnt_options); the trailing zero padding is harmless ("defaults").
     *
     * The padding matters because a kext build can copyin() a larger struct
     * than this small sfsmount_args_t. Against a 4-byte stack/heap object that
     * over-read walks off the end of the allocation and faults with EFAULT when
     * the bytes past it are unmapped. A full page is always large enough and is
     * trivially mapped and zeroed in BSS, so this works against both an
     * over-reading kext and a correct one. (See the procfs sibling for the
     * incident that established this.)
     */
    static unsigned char mount_data[4096];   /* zeroed (BSS) */
    ((sfsmount_args_t *)mount_data)->mnt_options = sysfs_options;

    char *mntdir = argv[1];
    if (verbose) {
        syslog(SYSFS_SYSLOG_LEVEL, "%s: Mounting sysfs on %s", prog_name, mntdir);
    }

    int result = mount("sysfs", mntdir, generic_options, mount_data);
    if (result < 0) {
        fprintf(stderr, "%s: Failed to mount sysfs on %s: %s\n", prog_name, mntdir, strerror(errno));
    }

    if (verbose) {
        if (result == 0) {
            syslog(SYSFS_SYSLOG_LEVEL, "%s: mount completed", prog_name);
        } else {
            syslog(SYSFS_SYSLOG_LEVEL, "%s: mount failed: %s", prog_name, strerror(errno));
        }
    }

    return result == 0 ? 0 : 1;
}

/*
 * Prints a usage message and exits.
 */
static void
usage(char *name) {
    fprintf(stderr, "%s: usage: %s [-o options] special mountpoint\n", name, name);
    fprintf(stderr, "Options are:\n");
    fprintf(stderr, "     -v\t\t\tEnables verbose logging of mount operation to syslog.\n");
    fprintf(stderr, "     -?, -h\t\tPrints this usage message and exits.\n");
    fprintf(stderr, "Example: mount -t %s %s /sys\n", "sysfs", "sysfs");

    exit(1);
    /* NOTREACHED */
}
