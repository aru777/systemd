/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering
  Copyright 2013 Zbigniew Jędrzejewski-Szmek

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <getopt.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "alloc-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "hashmap.h"
#include "locale-util.h"
#include "log.h"
#include "pager.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "signal-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "util.h"

static const char prefixes[] =
        "/etc\0"
        "/run\0"
        "/usr/local/lib\0"
        "/usr/local/share\0"
        "/usr/lib\0"
        "/usr/share\0"
#if HAVE_SPLIT_USR
        "/lib\0"
#endif
        ;

static const char suffixes[] =
        "sysctl.d\0"
        "tmpfiles.d\0"
        "modules-load.d\0"
        "binfmt.d\0"
        "systemd/system\0"
        "systemd/user\0"
        "systemd/system-preset\0"
        "systemd/user-preset\0"
        "udev/rules.d\0"
        "modprobe.d\0";

static const char have_dropins[] =
        "systemd/system\0"
        "systemd/user\0";

static bool arg_no_pager = false;
static int arg_diff = -1;

static enum {
        SHOW_MASKED = 1 << 0,
        SHOW_EQUIVALENT = 1 << 1,
        SHOW_REDIRECTED = 1 << 2,
        SHOW_OVERRIDDEN = 1 << 3,
        SHOW_UNCHANGED = 1 << 4,
        SHOW_EXTENDED = 1 << 5,

        SHOW_DEFAULTS =
        (SHOW_MASKED | SHOW_EQUIVALENT | SHOW_REDIRECTED | SHOW_OVERRIDDEN | SHOW_EXTENDED)
} arg_flags = 0;

static int equivalent(const char *a, const char *b) {
        _cleanup_free_ char *x = NULL, *y = NULL;
        int r;

        r = chase_symlinks(a, NULL, 0, &x);
        if (r < 0)
                return r;

        r = chase_symlinks(b, NULL, 0, &y);
        if (r < 0)
                return r;

        return path_equal(x, y);
}

static int notify_override_masked(const char *top, const char *bottom) {
        if (!(arg_flags & SHOW_MASKED))
                return 0;

        printf("%s%s%s     %s %s %s\n",
               ansi_highlight_red(), "[MASKED]", ansi_normal(),
               top, special_glyph(ARROW), bottom);
        return 1;
}

static int notify_override_equivalent(const char *top, const char *bottom) {
        if (!(arg_flags & SHOW_EQUIVALENT))
                return 0;

        printf("%s%s%s %s %s %s\n",
               ansi_highlight_green(), "[EQUIVALENT]", ansi_normal(),
               top, special_glyph(ARROW), bottom);
        return 1;
}

static int notify_override_redirected(const char *top, const char *bottom) {
        if (!(arg_flags & SHOW_REDIRECTED))
                return 0;

        printf("%s%s%s %s %s %s\n",
               ansi_highlight(), "[REDIRECTED]", ansi_normal(),
               top, special_glyph(ARROW), bottom);
        return 1;
}

static int notify_override_overridden(const char *top, const char *bottom) {
        if (!(arg_flags & SHOW_OVERRIDDEN))
                return 0;

        printf("%s%s%s %s %s %s\n",
               ansi_highlight(), "[OVERRIDDEN]", ansi_normal(),
               top, special_glyph(ARROW), bottom);
        return 1;
}

static int notify_override_extended(const char *top, const char *bottom) {
        if (!(arg_flags & SHOW_EXTENDED))
               return 0;

        printf("%s%s%s   %s %s %s\n",
               ansi_highlight(), "[EXTENDED]", ansi_normal(),
               top, special_glyph(ARROW), bottom);
        return 1;
}

static int notify_override_unchanged(const char *f) {
        if (!(arg_flags & SHOW_UNCHANGED))
                return 0;

        printf("[UNCHANGED]  %s\n", f);
        return 1;
}

static int found_override(const char *top, const char *bottom) {
        _cleanup_free_ char *dest = NULL;
        pid_t pid;
        int r;

        assert(top);
        assert(bottom);

        if (null_or_empty_path(top) > 0)
                return notify_override_masked(top, bottom);

        r = readlink_malloc(top, &dest);
        if (r >= 0) {
                if (equivalent(dest, bottom) > 0)
                        return notify_override_equivalent(top, bottom);
                else
                        return notify_override_redirected(top, bottom);
        }

        r = notify_override_overridden(top, bottom);
        if (!arg_diff)
                return r;

        putchar('\n');

        fflush(stdout);

        r = safe_fork("(diff)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_CLOSE_ALL_FDS|FORK_LOG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                execlp("diff", "diff", "-us", "--", bottom, top, NULL);
                log_open();
                log_error_errno(errno, "Failed to execute diff: %m");
                _exit(EXIT_FAILURE);
        }

        (void) wait_for_terminate_and_check("diff", pid, WAIT_LOG_ABNORMAL);
        putchar('\n');

        return r;
}

static int enumerate_dir_d(
                OrderedHashmap *top,
                OrderedHashmap *bottom,
                OrderedHashmap *drops,
                const char *toppath, const char *drop) {

        _cleanup_free_ char *unit = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_strv_free_ char **list = NULL;
        char **file;
        char *c;
        int r;

        assert(!endswith(drop, "/"));

        path = strjoin(toppath, "/", drop);
        if (!path)
                return -ENOMEM;

        log_debug("Looking at %s", path);

        unit = strdup(drop);
        if (!unit)
                return -ENOMEM;

        c = strrchr(unit, '.');
        if (!c)
                return -EINVAL;
        *c = 0;

        r = get_files_in_directory(path, &list);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate %s: %m", path);

        strv_sort(list);

        STRV_FOREACH(file, list) {
                OrderedHashmap *h;
                int k;
                char *p;
                char *d;

                if (!endswith(*file, ".conf"))
                        continue;

                p = strjoin(path, "/", *file);
                if (!p)
                        return -ENOMEM;
                d = p + strlen(toppath) + 1;

                log_debug("Adding at top: %s %s %s", d, special_glyph(ARROW), p);
                k = ordered_hashmap_put(top, d, p);
                if (k >= 0) {
                        p = strdup(p);
                        if (!p)
                                return -ENOMEM;
                        d = p + strlen(toppath) + 1;
                } else if (k != -EEXIST) {
                        free(p);
                        return k;
                }

                log_debug("Adding at bottom: %s %s %s", d, special_glyph(ARROW), p);
                free(ordered_hashmap_remove(bottom, d));
                k = ordered_hashmap_put(bottom, d, p);
                if (k < 0) {
                        free(p);
                        return k;
                }

                h = ordered_hashmap_get(drops, unit);
                if (!h) {
                        h = ordered_hashmap_new(&string_hash_ops);
                        if (!h)
                                return -ENOMEM;
                        ordered_hashmap_put(drops, unit, h);
                        unit = strdup(unit);
                        if (!unit)
                                return -ENOMEM;
                }

                p = strdup(p);
                if (!p)
                        return -ENOMEM;

                log_debug("Adding to drops: %s %s %s %s %s",
                          unit, special_glyph(ARROW), basename(p), special_glyph(ARROW), p);
                k = ordered_hashmap_put(h, basename(p), p);
                if (k < 0) {
                        free(p);
                        if (k != -EEXIST)
                                return k;
                }
        }
        return 0;
}

static int enumerate_dir(
                OrderedHashmap *top,
                OrderedHashmap *bottom,
                OrderedHashmap *drops,
                const char *path, bool dropins) {

        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        _cleanup_strv_free_ char **files = NULL, **dirs = NULL;
        size_t n_files = 0, allocated_files = 0, n_dirs = 0, allocated_dirs = 0;
        char **t;
        int r;

        assert(top);
        assert(bottom);
        assert(drops);
        assert(path);

        log_debug("Looking at %s", path);

        d = opendir(path);
        if (!d) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open %s: %m", path);
        }

        FOREACH_DIRENT_ALL(de, d, return -errno) {
                dirent_ensure_type(d, de);

                if (dropins && de->d_type == DT_DIR && endswith(de->d_name, ".d")) {
                        if (!GREEDY_REALLOC0(dirs, allocated_dirs, n_dirs + 2))
                                return -ENOMEM;

                        dirs[n_dirs] = strdup(de->d_name);
                        if (!dirs[n_dirs])
                                return -ENOMEM;
                        n_dirs ++;
                }

                if (!dirent_is_file(de))
                        continue;

                if (!GREEDY_REALLOC0(files, allocated_files, n_files + 2))
                        return -ENOMEM;

                files[n_files] = strdup(de->d_name);
                if (!files[n_files])
                        return -ENOMEM;
                n_files ++;
        }

        strv_sort(dirs);
        strv_sort(files);

        STRV_FOREACH(t, dirs) {
                r = enumerate_dir_d(top, bottom, drops, path, *t);
                if (r < 0)
                        return r;
        }

        STRV_FOREACH(t, files) {
                _cleanup_free_ char *p = NULL;

                p = strjoin(path, "/", *t);
                if (!p)
                        return -ENOMEM;

                log_debug("Adding at top: %s %s %s", basename(p), special_glyph(ARROW), p);
                r = ordered_hashmap_put(top, basename(p), p);
                if (r >= 0) {
                        p = strdup(p);
                        if (!p)
                                return -ENOMEM;
                } else if (r != -EEXIST)
                        return r;

                log_debug("Adding at bottom: %s %s %s", basename(p), special_glyph(ARROW), p);
                free(ordered_hashmap_remove(bottom, basename(p)));
                r = ordered_hashmap_put(bottom, basename(p), p);
                if (r < 0)
                        return r;
                p = NULL;
        }

        return 0;
}

static bool should_skip_path(const char *prefix, const char *suffix) {
#if HAVE_SPLIT_USR
        _cleanup_free_ char *target = NULL;
        const char *p;
        char *dirname;

        dirname = strjoina(prefix, "/", suffix);

        if (chase_symlinks(dirname, NULL, 0, &target) < 0)
                return false;

        NULSTR_FOREACH(p, prefixes) {
                if (path_startswith(dirname, p))
                        continue;

                if (path_equal(target, strjoina(p, "/", suffix))) {
                        log_debug("%s redirects to %s, skipping.", dirname, target);
                        return true;
                }
        }
#endif
        return false;
}

static int process_suffix(const char *suffix, const char *onlyprefix) {
        const char *p;
        char *f;
        OrderedHashmap *top, *bottom, *drops;
        OrderedHashmap *h;
        char *key;
        int r = 0, k;
        Iterator i, j;
        int n_found = 0;
        bool dropins;

        assert(suffix);
        assert(!startswith(suffix, "/"));
        assert(!strstr(suffix, "//"));

        dropins = nulstr_contains(have_dropins, suffix);

        top = ordered_hashmap_new(&string_hash_ops);
        bottom = ordered_hashmap_new(&string_hash_ops);
        drops = ordered_hashmap_new(&string_hash_ops);
        if (!top || !bottom || !drops) {
                r = -ENOMEM;
                goto finish;
        }

        NULSTR_FOREACH(p, prefixes) {
                _cleanup_free_ char *t = NULL;

                if (should_skip_path(p, suffix))
                        continue;

                t = strjoin(p, "/", suffix);
                if (!t) {
                        r = -ENOMEM;
                        goto finish;
                }

                k = enumerate_dir(top, bottom, drops, t, dropins);
                if (r == 0)
                        r = k;
        }

        ORDERED_HASHMAP_FOREACH_KEY(f, key, top, i) {
                char *o;

                o = ordered_hashmap_get(bottom, key);
                assert(o);

                if (!onlyprefix || startswith(o, onlyprefix)) {
                        if (path_equal(o, f)) {
                                notify_override_unchanged(f);
                        } else {
                                k = found_override(f, o);
                                if (k < 0)
                                        r = k;
                                else
                                        n_found += k;
                        }
                }

                h = ordered_hashmap_get(drops, key);
                if (h)
                        ORDERED_HASHMAP_FOREACH(o, h, j)
                                if (!onlyprefix || startswith(o, onlyprefix))
                                        n_found += notify_override_extended(f, o);
        }

finish:
        ordered_hashmap_free_free(top);
        ordered_hashmap_free_free(bottom);

        ORDERED_HASHMAP_FOREACH_KEY(h, key, drops, i) {
                ordered_hashmap_free_free(ordered_hashmap_remove(drops, key));
                ordered_hashmap_remove(drops, key);
                free(key);
        }
        ordered_hashmap_free(drops);

        return r < 0 ? r : n_found;
}

static int process_suffixes(const char *onlyprefix) {
        const char *n;
        int n_found = 0, r;

        NULSTR_FOREACH(n, suffixes) {
                r = process_suffix(n, onlyprefix);
                if (r < 0)
                        return r;

                n_found += r;
        }

        return n_found;
}

static int process_suffix_chop(const char *arg) {
        const char *p;

        assert(arg);

        if (!path_is_absolute(arg))
                return process_suffix(arg, NULL);

        /* Strip prefix from the suffix */
        NULSTR_FOREACH(p, prefixes) {
                const char *suffix;

                suffix = startswith(arg, p);
                if (suffix) {
                        suffix += strspn(suffix, "/");
                        if (*suffix)
                                return process_suffix(suffix, p);
                        else
                                return process_suffixes(arg);
                }
        }

        log_error("Invalid suffix specification %s.", arg);
        return -EINVAL;
}

static void help(void) {
        printf("%s [OPTIONS...] [SUFFIX...]\n\n"
               "Find overridden configuration files.\n\n"
               "  -h --help           Show this help\n"
               "     --version        Show package version\n"
               "     --no-pager       Do not pipe output into a pager\n"
               "     --diff[=1|0]     Show a diff when overridden files differ\n"
               "  -t --type=LIST...   Only display a selected set of override types\n"
               , program_invocation_short_name);
}

static int parse_flags(const char *flag_str, int flags) {
        const char *word, *state;
        size_t l;

        FOREACH_WORD_SEPARATOR(word, l, flag_str, ",", state) {
                if (strneq("masked", word, l))
                        flags |= SHOW_MASKED;
                else if (strneq ("equivalent", word, l))
                        flags |= SHOW_EQUIVALENT;
                else if (strneq("redirected", word, l))
                        flags |= SHOW_REDIRECTED;
                else if (strneq("overridden", word, l))
                        flags |= SHOW_OVERRIDDEN;
                else if (strneq("unchanged", word, l))
                        flags |= SHOW_UNCHANGED;
                else if (strneq("extended", word, l))
                        flags |= SHOW_EXTENDED;
                else if (strneq("default", word, l))
                        flags |= SHOW_DEFAULTS;
                else
                        return -EINVAL;
        }
        return flags;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_NO_PAGER = 0x100,
                ARG_DIFF,
                ARG_VERSION
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'          },
                { "version",   no_argument,       NULL, ARG_VERSION  },
                { "no-pager",  no_argument,       NULL, ARG_NO_PAGER },
                { "diff",      optional_argument, NULL, ARG_DIFF     },
                { "type",      required_argument, NULL, 't'          },
                {}
        };

        int c;

        assert(argc >= 1);
        assert(argv);

        while ((c = getopt_long(argc, argv, "ht:", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case 't': {
                        int f;
                        f = parse_flags(optarg, arg_flags);
                        if (f < 0) {
                                log_error("Failed to parse flags field.");
                                return -EINVAL;
                        }
                        arg_flags = f;
                        break;
                }

                case ARG_DIFF:
                        if (!optarg)
                                arg_diff = 1;
                        else {
                                int b;

                                b = parse_boolean(optarg);
                                if (b < 0) {
                                        log_error("Failed to parse diff boolean.");
                                        return -EINVAL;
                                }

                                arg_diff = b;
                        }
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        return 1;
}

int main(int argc, char *argv[]) {
        int r, k, n_found = 0;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (arg_flags == 0)
                arg_flags = SHOW_DEFAULTS;

        if (arg_diff < 0)
                arg_diff = !!(arg_flags & SHOW_OVERRIDDEN);
        else if (arg_diff)
                arg_flags |= SHOW_OVERRIDDEN;

        (void) pager_open(arg_no_pager, false);

        if (optind < argc) {
                int i;

                for (i = optind; i < argc; i++) {
                        path_kill_slashes(argv[i]);

                        k = process_suffix_chop(argv[i]);
                        if (k < 0)
                                r = k;
                        else
                                n_found += k;
                }

        } else {
                k = process_suffixes(NULL);
                if (k < 0)
                        r = k;
                else
                        n_found += k;
        }

        if (r >= 0)
                printf("%s%i overridden configuration files found.\n", n_found ? "\n" : "", n_found);

finish:
        pager_close();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
