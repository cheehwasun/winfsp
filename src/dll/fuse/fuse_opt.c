/**
 * @file dll/fuse/fuse_opt.c
 *
 * @copyright 2015-2016 Bill Zissimopoulos
 */
/*
 * This file is part of WinFsp.
 *
 * You can redistribute it and/or modify it under the terms of the
 * GNU Affero General Public License version 3 as published by the
 * Free Software Foundation.
 *
 * Licensees holding a valid commercial license may use this file in
 * accordance with the commercial license agreement provided with the
 * software.
 */

#include <dll/library.h>
#include <fuse/fuse_opt.h>
#include <stdint.h>

#define fsp_fuse_opt_match_none         ((const char *)0)   /* no option match */
#define fsp_fuse_opt_match_exact        ((const char *)1)   /* exact option match */
#define fsp_fuse_opt_match_next         ((const char *)2)   /* option match, value is next arg */

static long long strtoint(const char *p, const char *endp, int base, int is_signed)
{
    long long v;
    int maxdig, maxalp, sign = +1;

    if (is_signed)
    {
        if ('+' == *p)
            p++;
        else if ('-' == *p)
            p++, sign = -1;
    }

    if (0 == base)
    {
        if ('0' == *p)
        {
            p++;
            if ('x' == *p || 'X' == *p)
            {
                p++;
                base = 16;
            }
            else
                base = 8;
        }
        else
        {
            base = 10;
        }
    }

    maxdig = 10 < base ? '9' : (base - 1) + '0';
    maxalp = 10 < base ? (base - 1 - 10) + 'a' : 0;

    for (v = 0; *p && endp != p; p++)
    {
        int c = *p;

        if ('0' <= c && c <= maxdig)
            v = 10 * v + (c - '0');
        else
        {
            c |= 0x20;
            if ('a' <= c && c <= maxalp)
                v = 10 * v + (c - 'a');
            else
                break;
        }
    }

    return sign * v;
}

static void fsp_fuse_opt_match_templ(
    const char *templ, const char **pspec,
    const char **parg, const char *argend)
{
    const char *p, *q;

    *pspec = 0;

    for (p = templ, q = *parg;; p++, q++)
        if ('\0' == *q || q == argend)
        {
            if ('\0' == *p)
                *parg = fsp_fuse_opt_match_exact;
            else if (' ' == *p)
                *pspec = p + 1, *parg = fsp_fuse_opt_match_next;
            else
                *parg = fsp_fuse_opt_match_none;
            break;
        }
        else if ('=' == *p)
        {
            if (*q == *p)
                *pspec = p + 1, *parg = q + 1;
            else
                *parg = fsp_fuse_opt_match_none;
            break;
        }
        else if (' ' == *p)
        {
            *pspec = p + 1, *parg = q;
            break;
        }
        else if (*q != *p)
        {
            *parg = fsp_fuse_opt_match_none;
            break;
        }
}

static const struct fuse_opt *fsp_fuse_opt_find(
    const struct fuse_opt opts[], const char **pspec,
    const char **parg, const char *argend)
{
    const struct fuse_opt *opt;
    const char *arg;

    for (opt = opts; 0 != opt->templ; opt++)
    {
        arg = *parg;
        fsp_fuse_opt_match_templ(opt->templ, pspec, &arg, argend);
        if (fsp_fuse_opt_match_none != arg)
        {
            *parg = arg;
            return opt;
        }
    }

    return 0;
}

static int fsp_fuse_opt_call_proc(void *data,
    fuse_opt_proc_t proc,
    const char *arg, const char *argend,
    int key, int is_opt,
    struct fuse_args *outargs,
    FSP_FUSE_MEMFN_P)
{
    int result;

    if (FUSE_OPT_KEY_DISCARD == key)
        return 0;

    if (0 != argend)
    {
        char *argcopy = memalloc(argend - arg + 1);
        if (0 == argcopy)
            return -1;
        memcpy(argcopy, arg, argend - arg);
        argcopy[argend - arg] = '\0';
        arg = argcopy;
    }

    if (FUSE_OPT_KEY_KEEP != key && 0 != proc)
    {
        result = proc(data, arg, key, outargs);
        if (-1 == result || 0 == result)
            goto exit;
    }

    if (is_opt)
    {
        if (!(3 <= outargs->argc &&
            '-' == outargs->argv[1][0] && 'o' == outargs->argv[1][1] &&
            '\0' == outargs->argv[1][2]))
        {
            result = fsp_fuse_opt_insert_arg(outargs, 1, "-o", FSP_FUSE_MEMFN_A);
            if (-1 == result)
                goto exit;
            result = fsp_fuse_opt_insert_arg(outargs, 2, "", FSP_FUSE_MEMFN_A);
            if (-1 == result)
                goto exit;
        }

        result = fsp_fuse_opt_add_opt_escaped(&outargs->argv[2], arg, FSP_FUSE_MEMFN_A);
        if (-1 == result)
            goto exit;
    }
    else
    {
        result = fsp_fuse_opt_add_arg(outargs, arg, FSP_FUSE_MEMFN_A);
        if (-1 == result)
            goto exit;
    }

exit:
    if (0 != argend)
        memfree((void *)arg); /* argcopy */

    return 0;
}

static int fsp_fuse_opt_process_arg(void *data,
    const struct fuse_opt *opt, fuse_opt_proc_t proc,
    const char *spec,
    const char *arg, const char *argend, const char *argl,
    int is_opt,
    struct fuse_args *outargs,
    FSP_FUSE_MEMFN_P)
{
#define VAR(data, opt, type)            *(type *)((char *)(data) + (opt)->offset)

    if (-1L == opt->offset)
        return fsp_fuse_opt_call_proc(data, proc, arg, argend, opt->value, is_opt, outargs,
            FSP_FUSE_MEMFN_A);
    else
    {
        int h, j, l, t, z;
        long long llv;
        char *s;
        int len;

        if (0 == spec || '\0' == spec[0])
        {
            VAR(data, opt, int) = opt->value;
            return 0;
        }

        if ('%' != spec[0])
            return -1; /* bad option template */

        h = j = l = t = z = 0;
        for (spec++; *spec; spec++)
            switch (*spec)
            {
            default:
            case 0: case 1: case 2: case 3: case 4:
            case 5: case 6: case 7: case 8: case 9:
            case 'm':
                break;
            case 'h':
                h++;
                break;
            case 'j':
                j++;
                break;
            case 'l':
                l++;
                break;
            case 'L': case 'q':
                l += 2;
                break;
            case 't':
                t++;
                break;
            case 'z':
                z++;
                break;
            case 'd':
                llv = strtoint(argl, argend, 10, 1);
                goto ivar;
            case 'i':
                llv = strtoint(argl, argend, 0, 1);
                goto ivar;
            case 'o':
                llv = strtoint(argl, argend, 8, 0);
                goto ivar;
            case 'u':
                llv = strtoint(argl, argend, 10, 0);
                goto ivar;
            case 'x': case 'X':
                llv = strtoint(argl, argend, 16, 0);
            ivar:
                if (z)
                    VAR(data, opt, size_t) = (size_t)llv;
                else if (t)
                    VAR(data, opt, ptrdiff_t) = (ptrdiff_t)llv;
                else if (j)
                    VAR(data, opt, intmax_t) = (intmax_t)llv;
                else if (1 == h)
                    VAR(data, opt, short) = (short)llv;
                else if (2 <= h)
                    VAR(data, opt, char) = (char)llv;
                else if (1 == l)
                    VAR(data, opt, long) = (long)llv;
                else if (2 <= l)
                    VAR(data, opt, long long) = (long long)llv;
                else
                    VAR(data, opt, int) = (int)llv;
                return 0;
            case 's': case 'c':
                if (arg <= argl && argl < argend)
                    len = (int)(argend - argl);
                else
                    len = lstrlenA(argl);
                s = memalloc(len + 1);
                if (0 == s)
                    return -1;
                memcpy(s, argl, len);
                s[len] = '\0';
                VAR(data, opt, const char *) = (const char *)s;
                return 0;
            case 'a': case 'e': case 'E': case 'f': case 'g':
                return -1; /* no float support */
            }

        return -1; /* bad option template */
    }

#undef VAR
}

static int fsp_fuse_opt_parse_arg(void *data,
    const struct fuse_opt opts[], fuse_opt_proc_t proc,
    const char *arg, const char *argend, const char *nextarg,
    int is_opt,
    struct fuse_args *outargs,
    FSP_FUSE_MEMFN_P)
{
    const struct fuse_opt *opt;
    const char *spec, *argl;
    int processed = 0;

    argl = arg;
    opt = opts;
    while (0 != (opt = fsp_fuse_opt_find(opt, &spec, &argl, argend)))
    {
        if (fsp_fuse_opt_match_exact == argl)
            argl = arg;
        else if (fsp_fuse_opt_match_next == argl)
        {
            if (0 == nextarg)
                return -1; /* missing argument for option */
            argl = nextarg;
        }

        if (-1 == fsp_fuse_opt_process_arg(data, opt, proc, spec, arg, argend, argl,
            is_opt, outargs, FSP_FUSE_MEMFN_A))
            return -1;
        processed++;

        argl = arg;
        opt++;
    }

    if (0 != processed)
        return 0;

    return fsp_fuse_opt_call_proc(data, proc, arg, argend, FUSE_OPT_KEY_OPT, is_opt, outargs,
        FSP_FUSE_MEMFN_A);
}

static int fsp_fuse_opt_proc0(void *data, const char *arg, int key,
    struct fuse_args *outargs)
{
    return 1;
}

FSP_FUSE_API int fsp_fuse_opt_parse(struct fuse_args *args, void *data,
    const struct fuse_opt opts[], fuse_opt_proc_t proc,
    FSP_FUSE_MEMFN_P)
{
    static struct fuse_args args0 = FUSE_ARGS_INIT(0, 0);
    static struct fuse_opt opts0[1] = { FUSE_OPT_END };
    struct fuse_args outargs = FUSE_ARGS_INIT(0, 0);
    const char *arg, *argend;
    int dashdash = 0;

    if (0 == args)
        args = &args0;
    if (0 == opts)
        opts = opts0;
    if (0 == proc)
        proc = fsp_fuse_opt_proc0;

    if (-1 == fsp_fuse_opt_add_arg(&outargs, args->argv[0], FSP_FUSE_MEMFN_A))
        return -1;

    for (int argi = 1; args->argc > argi; argi++)
    {
        arg = args->argv[argi];
        if ('-' == arg[0] && !dashdash)
        {
            switch (arg[1])
            {
            case 'o':
                if ('\0' == arg[2])
                {
                    if (args->argc <= argi + 1)
                        goto fail; /* missing argument for option "-o" */
                    arg = args->argv[++argi];
                }
                else
                    arg += 2;
                for (argend = arg; *arg; argend++)
                {
                    if ('\0' == *argend || ',' == *argend)
                    {
                        if (-1 == fsp_fuse_opt_parse_arg(data, opts, proc,
                            arg, argend, 0, 1, &outargs, FSP_FUSE_MEMFN_A))
                            goto fail;

                        arg = '\0' == *argend ? argend : argend + 1;
                    }
                    else if ('\\' == *argend && '\0' != argend[1])
                        argend++;
                }
                break;
            case '-':
                if ('\0' == arg[2])
                {
                    if (-1 == fsp_fuse_opt_add_arg(&outargs, arg, FSP_FUSE_MEMFN_A))
                        return -1;
                    dashdash = 1;
                    break;
                }
                /* fall through */
            default:
                if (-1 == fsp_fuse_opt_parse_arg(data, opts, proc,
                    arg, 0, args->argv[argi + 1], 0, &outargs, FSP_FUSE_MEMFN_A))
                    goto fail;
                break;
            }
        }
        else
            if (-1 == fsp_fuse_opt_call_proc(data, proc, arg, 0, FUSE_OPT_KEY_NONOPT, 0, &outargs,
                FSP_FUSE_MEMFN_A))
                goto fail;
    }

    /* if "--" is the last argument, remove it (fuse_opt compatibility) */
    if (0 < outargs.argc &&
        '-' == outargs.argv[outargs.argc - 1][0] &&
        '-' == outargs.argv[outargs.argc - 1][1] &&
        '\0' == outargs.argv[outargs.argc - 1][2])
    {
        memfree(outargs.argv[--outargs.argc]);
        outargs.argv[outargs.argc] = 0;
    }

    return 0;

fail:
    fsp_fuse_opt_free_args(&outargs, FSP_FUSE_MEMFN_A);

    return -1;
}

FSP_FUSE_API int fsp_fuse_opt_add_arg(struct fuse_args *args, const char *arg,
    FSP_FUSE_MEMFN_P)
{
    return fsp_fuse_opt_insert_arg(args, args->argc, arg, FSP_FUSE_MEMFN_A);
}

FSP_FUSE_API int fsp_fuse_opt_insert_arg(struct fuse_args *args, int pos, const char *arg,
    FSP_FUSE_MEMFN_P)
{
    char **argv;
    int argsize;

    if (0 == args)
        return -1;
    if (0 != args->argv && !args->allocated)
        return -1;
    if (0 > pos || pos > args->argc)
        return -1;

    argv = memalloc((args->argc + 2) * sizeof(char *));
    if (0 == argv)
        return -1;
    argsize = lstrlenA(arg) + 1;
    argv[pos] = memalloc(argsize);
    if (0 == argv[pos])
    {
        memfree(argv);
        return -1;
    }

    memcpy(argv[pos], arg, argsize);
    memcpy(argv, args->argv, sizeof(char *) * pos);
    memcpy(argv + pos + 1, args->argv + pos, sizeof(char *) * (args->argc - pos));

    memfree(args->argv);

    args->argc++;
    args->argv = argv;
    argv[args->argc] = 0;
    args->allocated = 1;

    return 0;
}

FSP_FUSE_API void fsp_fuse_opt_free_args(struct fuse_args *args,
    FSP_FUSE_MEMFN_P)
{
    if (0 == args)
        return;

    if (args->allocated && 0 != args->argv)
    {
        for (int argi = 0; args->argc > argi; argi++)
            memfree(args->argv[argi]);

        memfree(args->argv);
    }

    args->argc = 0;
    args->argv = 0;
    args->allocated = 0;
}

static int fsp_fuse_opt_add_opt_internal(char **opts, const char *opt, int escaped,
    FSP_FUSE_MEMFN_P)
{
    size_t optsize, optlen;
    char *newopts;
    const char *p;

    optsize = 0 != *opts && '\0' != (*opts)[0] ? lstrlenA(*opts) + 1 : 0;
    for (p = opt, optlen = 0; *p; p++, optlen++)
        if (escaped && (',' == *p || '\\' == *p))
            optlen++;

    newopts = memalloc(optsize + optlen + 1);
    if (0 == newopts)
        return -1;

    if (0 != optsize)
    {
        memcpy(newopts, *opts, optsize - 1);
        newopts[optsize - 1] = ',';
    }

    memfree(*opts);
    *opts = newopts;
    newopts += optsize;

    for (p = opt; *p; p++, newopts++)
    {
        if (escaped && (',' == *p || '\\' == *p))
            *newopts++ = '\\';
        *newopts = *p;
    }
    *newopts = '\0';

    return 0;
}

FSP_FUSE_API int fsp_fuse_opt_add_opt(char **opts, const char *opt,
    FSP_FUSE_MEMFN_P)
{
    return fsp_fuse_opt_add_opt_internal(opts, opt, 0,
        FSP_FUSE_MEMFN_A);
}

FSP_FUSE_API int fsp_fuse_opt_add_opt_escaped(char **opts, const char *opt,
    FSP_FUSE_MEMFN_P)
{
    return fsp_fuse_opt_add_opt_internal(opts, opt, 1,
        FSP_FUSE_MEMFN_A);
}

FSP_FUSE_API int fsp_fuse_opt_match(const struct fuse_opt opts[], const char *arg,
    FSP_FUSE_MEMFN_P)
{
    if (0 == opts)
        return 0;

    const char *spec;
    return fsp_fuse_opt_find(opts, &spec, &arg, 0) ? 1 : 0;
}