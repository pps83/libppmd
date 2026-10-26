#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <getopt.h>
#include "Ppmd8.h"

#define IS_TTY(f)     isatty(fileno(f))

#define PPMDI_EXT        ".ppmd"
#define PPMDI_VERSION    "1.0"
#define PPMDI_RESTORE    0
#define PPMD_BUFFER_SIZE 65536
#define MAGIC            0x84ACAF8F

static void *pmalloc(ISzAllocPtr ip, size_t size)
{
    (void) ip;
    return malloc(size);
}

static void pfree(ISzAllocPtr ip, void *addr)
{
    (void) ip;
    free(addr);
}

static ISzAlloc ialloc = { pmalloc, pfree };

struct CharWriter {
    void (*Write)(void *p, Byte b);
    FILE *fp;
};

struct CharReader {
    Byte (*Read)(void *p);
    FILE *fp;
    bool eof;
};

static void Write(void *p, Byte b)
{
    struct CharWriter *cw = p;
    putc_unlocked(b, cw->fp);
}

static Byte Read(void *p)
{
    struct CharReader *cr = p;
    if (cr->eof)
        return 0;
    int c = getc_unlocked(cr->fp);
    if (c == EOF) {
        cr->eof = true;
        return 0;
    }
    return (Byte)c;
}

struct header {
    unsigned magic;
    unsigned short info, fnlen;
};

static const char *progname = "ppmdi";
static const char *partial_output = NULL;

struct Options {
    bool decompress;
    bool tostdout;
    bool keep;
    bool force;
    bool verbose;
    bool quiet;
    int mem;
    int order;
};

static struct Options opt = {
    .decompress = false,
    .tostdout   = false,
    .keep       = false,
    .force      = false,
    .verbose    = false,
    .quiet      = false,
    .mem        = 64,
    .order      = 4,
};

static void errmsg(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void warnmsg(const char *fmt, ...) {
    if (opt.quiet) return;
    va_list ap;
    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig) {
    if (partial_output) remove(partial_output);
    signal(sig, SIG_DFL);
    raise(sig);
}

static bool compress_stream(FILE* in, FILE* out, const char* fname)
{
    struct header hdr = { MAGIC, 0, 1 };

    // Extract base name safely (Unix forward slash path separator)
    const char *fname2 = fname;
    const char *spos = strrchr(fname2, '/');
    if (spos) fname2 = spos + 1;
    int baseLen = (int)strlen(fname2);

    hdr.info = (unsigned short)((opt.order - 1) | ((opt.mem - 1) << 4) | (('I' - 'A') << 12));
    hdr.fnlen = (unsigned short)baseLen;

    if (fwrite(&hdr, sizeof hdr, 1, out) != 1) return false;
    if (fwrite(fname2, 1, (size_t)baseLen, out) != (size_t)baseLen) return false;

    struct CharWriter cw = { Write, out };
    CPpmd8 ppmd;
    memset(&ppmd, 0, sizeof(ppmd));
    ppmd.Stream.Out = (IByteOut *) &cw;

    Ppmd8_Construct(&ppmd);
    if (!Ppmd8_Alloc(&ppmd, (UInt32)(opt.mem << 20), &ialloc)) {
        errmsg("out of memory for PPMd allocation");
        return false;
    }
    Ppmd8_Init_RangeEnc(&ppmd);
    Ppmd8_Init(&ppmd, (unsigned)opt.order, (unsigned)PPMDI_RESTORE);

    unsigned char buf[PPMD_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        for (size_t i = 0; i < n; i++)
            Ppmd8_EncodeSymbol(&ppmd, buf[i]);
    }
    if (ferror(in)) { errmsg("read error"); Ppmd8_Free(&ppmd, &ialloc); return false; }

    Ppmd8_EncodeSymbol(&ppmd, -1); /* EndMark */
    Ppmd8_Flush_RangeEnc(&ppmd);
    Ppmd8_Free(&ppmd, &ialloc);
    return fflush(out) == 0 && !ferror(out);
}

static bool decompress_stream(FILE* in, FILE* out)
{
    struct header hdr;
    if (fread(&hdr, sizeof hdr, 1, in) != 1) { errmsg("truncated or invalid file header"); return false; }
    if (hdr.magic != MAGIC) { errmsg("not a valid ppmdi file (bad magic)"); return false; }
    if (hdr.info >> 12 != 'I' - 'A') { errmsg("invalid header format signature"); return false; }

    char fname[0x200];
    size_t fnlen = hdr.fnlen & 0x1FF;
    if (fnlen >= sizeof(fname)) { errmsg("filename in header too long"); return false; }
    if (fread(fname, 1, fnlen, in) != fnlen) { errmsg("truncated filename field"); return false; }
    fname[fnlen] = '\0';

    opt.order = (int)(hdr.info & 0xf) + 1;
    opt.mem = (int)((hdr.info >> 4) & 0xff) + 1;

    struct CharReader cr = { Read, in, false };
    CPpmd8 ppmd;
    memset(&ppmd, 0, sizeof(ppmd));
    ppmd.Stream.In = (IByteIn *) &cr;

    Ppmd8_Construct(&ppmd);
    if (!Ppmd8_Alloc(&ppmd, (UInt32)(opt.mem << 20), &ialloc)) {
        errmsg("out of memory for PPMd allocation");
        return false;
    }
    Ppmd8_Init_RangeDec(&ppmd);
    Ppmd8_Init(&ppmd, (unsigned)opt.order, (unsigned)PPMDI_RESTORE);

    unsigned char buf[PPMD_BUFFER_SIZE];
    size_t n = 0;
    int c = 0;
    while (true) {
        c = Ppmd8_DecodeSymbol(&ppmd);
        if (cr.eof || c < 0)
            break;
        buf[n++] = (unsigned char)c;
        if (n == sizeof buf) {
            if (out && fwrite(buf, 1, sizeof buf, out) != sizeof buf) {
                errmsg("write error");
                Ppmd8_Free(&ppmd, &ialloc);
                return false;
            }
            n = 0;
        }
    }
    if (n && out) {
        if (fwrite(buf, 1, n, out) != n) {
            errmsg("write error");
            Ppmd8_Free(&ppmd, &ialloc);
            return false;
        }
    }

    bool ok = (c == -1 && Ppmd8_RangeDec_IsFinishedOK(&ppmd) && !ferror(in));
    if (!ok) errmsg("decompression stream integrity check failed");
    Ppmd8_Free(&ppmd, &ialloc);
    return ok;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static int do_file(const char *name) {
    const bool use_stdin = (name == NULL);
    const bool to_std = opt.tostdout || use_stdin;
    FILE *in = stdin, *out = NULL;
    char *outname = NULL;
    struct stat st;
    bool have_st = false;

    memset(&st, 0, sizeof st);
    if (!use_stdin) {
        if (stat(name, &st) != 0) { errmsg("%s: %s", name, strerror(errno)); return 1; }
        have_st = true;
        if ((st.st_mode & S_IFMT) == S_IFDIR) { warnmsg("%s is a directory -- ignored", name); return 2; }
        if ((st.st_mode & S_IFMT) != S_IFREG && !opt.tostdout) {
            warnmsg("%s is not a regular file -- ignored", name);
            return 2;
        }
        if (!to_std) {
            size_t ln = strlen(name), lx = strlen(PPMDI_EXT);
            if (opt.decompress) {
                if (!ends_with(name, PPMDI_EXT) || ln == lx) {
                    warnmsg("%s: unknown suffix -- ignored", name);
                    return 2;
                }
                outname = (char*)malloc(ln - lx + 1);
                if (outname) { memcpy(outname, name, ln - lx); outname[ln - lx] = 0; }
            } else {
                if (ends_with(name, PPMDI_EXT)) {
                    warnmsg("%s already has %s suffix -- unchanged", name, PPMDI_EXT);
                    return 2;
                }
                outname = (char*)malloc(ln + lx + 1);
                if (outname) { memcpy(outname, name, ln); memcpy(outname + ln, PPMDI_EXT, lx + 1); }
            }
            if (!outname) { errmsg("out of memory"); return 1; }
        }
        in = fopen(name, "rb");
        if (!in) { errmsg("%s: %s", name, strerror(errno)); free(outname); return 1; }
    }

    const char *label = use_stdin ? "stdin" : name;

    if (opt.decompress && in == stdin && IS_TTY(stdin) && !opt.force) {
        errmsg("compressed data not read from a terminal. Use -f to force decompression.");
        free(outname);
        return 1;
    }
    if (!opt.decompress && to_std && IS_TTY(stdout) && !opt.force) {
        errmsg("compressed data not written to a terminal. Use -f to force compression.");
        if (in != stdin) fclose(in);
        free(outname);
        return 1;
    }

    if (to_std) {
        out = stdout;
    } else {
        struct stat ost;
        if (stat(outname, &ost) == 0 && !opt.force) {
            bool overwrite = false;
            if (IS_TTY(stdin) && IS_TTY(stderr)) {
                fprintf(stderr, "%s: %s already exists; do you wish to overwrite (y or n)? ", progname, outname);
                int ch = getchar(), skip = ch;
                while (skip != '\n' && skip != EOF) skip = getchar();
                overwrite = (ch == 'y' || ch == 'Y');
            }
            if (!overwrite) {
                warnmsg("%s already exists -- not overwritten", outname);
                fclose(in);
                free(outname);
                return 2;
            }
        }
        out = fopen(outname, "wb");
        if (!out) {
            errmsg("%s: %s", outname, strerror(errno));
            fclose(in);
            free(outname);
            return 1;
        }
        partial_output = outname;
    }

    bool ok = opt.decompress ? decompress_stream(in, out)
                             : compress_stream(in, out, use_stdin ? "stdin" : name);

    if (out == stdout) {
        if (fflush(stdout) != 0 || ferror(stdout)) { errmsg("write error: %s", strerror(errno)); ok = false; }
    } else if (out) {
        bool werr = ferror(out) != 0;
        if (fclose(out) != 0) werr = true;
        if (werr) { errmsg("%s: write error: %s", outname, strerror(errno)); ok = false; }
    }
    if (in != stdin) fclose(in);

    if (!ok) {
        if (outname) remove(outname);
        partial_output = NULL;
        free(outname);
        return 1;
    }

    if (opt.verbose) {
        fprintf(stderr, "%s:\t", label);
        if (outname) fprintf(stderr, " -- %s %s", opt.keep ? "created" : "replaced with", outname);
        fprintf(stderr, "\n");
    }

    if (outname) {
        if (have_st) {
            chmod(outname, st.st_mode & 07777);
            struct utimbuf ub;
            ub.actime = st.st_atime;
            ub.modtime = st.st_mtime;
            utime(outname, &ub);
        }
        partial_output = NULL;
        if (!opt.keep && remove(name) != 0) warnmsg("%s: cannot remove: %s", name, strerror(errno));
        free(outname);
    }
    return 0;
}

static void usage(FILE *f) {
    fprintf(f,
        "PPMDI %s\n"
        "Usage: %s [OPTION]... [FILE]...\n"
        "\n"
        "  -c, --stdout       write on standard output, keep original files unchanged\n"
        "  -d, --decompress   decompress\n"
        "  -k, --keep         keep (don't delete) input files\n"
        "  -f, --force        overwrite existing output files; allow terminal I/O\n"
        "  -m, --memory=N     set memory size in MB (default 64)\n"
        "  -o, --order=N      set model order (default 4)\n"
        "  -v, --verbose      verbose mode\n"
        "  -q, --quiet        suppress warnings\n"
        "  -h, --help         give this help\n"
        "  -V, --version      display version number\n",
        PPMDI_VERSION, progname);
}

int main(int argc, char **argv)
{
    static char pn[64];
    const char *base = argv[0];
    for (const char *s = argv[0]; *s; ++s) if (*s == '/') base = s + 1;
    strncpy(pn, base, sizeof pn - 1);
    size_t lpn = strlen(pn);
    if (lpn > 4 && strcmp(pn + lpn - 4, ".exe") == 0) pn[lpn - 4] = 0;
    if (pn[0]) progname = pn;
    if (strncmp(pn, "un", 2) == 0) opt.decompress = true;
    if (strstr(pn, "cat")) { opt.decompress = true; opt.tostdout = true; }

    static const struct option longopts[] = {
        { "stdout",     0, NULL, 'c' },
        { "decompress", 0, NULL, 'd' },
        { "uncompress", 0, NULL, 'd' },
        { "keep",       0, NULL, 'k' },
        { "force",      0, NULL, 'f' },
        { "memory",     1, NULL, 'm' },
        { "order",      1, NULL, 'o' },
        { "verbose",    0, NULL, 'v' },
        { "quiet",      0, NULL, 'q' },
        { "help",       0, NULL, 'h' },
        { "version",    0, NULL, 'V' },
        {  NULL,        0, NULL,  0  }
    };

    const char **files = (const char**)malloc(sizeof(char*) * (size_t)(argc + 1));
    int nfiles = 0;
    if (!files) { errmsg("out of memory"); return 1; }

    int opt_c_char;
    while ((opt_c_char = getopt_long(argc, argv, "cdkfvm:o:hqV", longopts, NULL)) != -1) {
        switch (opt_c_char) {
            case 'c': opt.tostdout = true; break;
            case 'd': opt.decompress = true; break;
            case 'k': opt.keep = true; break;
            case 'f': opt.force = true; break;
            case 'm': opt.mem = atoi(optarg); break;
            case 'o': opt.order = atoi(optarg); break;
            case 'v': opt.verbose = true; break;
            case 'q': opt.quiet = true; break;
            case 'h': usage(stdout); free(files); return 0;
            case 'V': printf("ppmdi %s\n", PPMDI_VERSION); free(files); return 0;
            default:
                usage(stderr);
                free(files);
                return 1;
        }
    }

    for (int i = optind; i < argc; ++i) {
        files[nfiles++] = argv[i];
    }

    if (!opt.decompress && opt.tostdout && nfiles > 1) {
        errmsg("cannot compress several files to stdout (concatenated streams are not supported)");
        free(files);
        return 1;
    }

    if (signal(SIGINT, SIG_IGN) != SIG_IGN) signal(SIGINT, on_signal);
    if (signal(SIGTERM, SIG_IGN) != SIG_IGN) signal(SIGTERM, on_signal);
#ifdef SIGHUP
    if (signal(SIGHUP, SIG_IGN) != SIG_IGN) signal(SIGHUP, on_signal);
#endif

    int status = 0;
    if (nfiles == 0) {
        status = do_file(NULL);
    } else {
        for (int i = 0; i < nfiles; ++i) {
            int r = do_file(strcmp(files[i], "-") == 0 ? NULL : files[i]);
            if (r == 1) status = 1;
            else if (r == 2 && status == 0) status = 2;
        }
    }
    free(files);
    return status;
}
