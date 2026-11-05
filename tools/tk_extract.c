/*
 * tk_extract.c -- get Textalker off an Apple II disk image.
 *
 * Until this existed, using EchoTalk meant installing CiderPress, a
 * Windows GUI, and picking two files out of a disk by hand. That is the
 * wrong shape for a screen-reader user and the wrong shape for an
 * installer, and it was the last thing standing between the project and
 * somebody else being able to run it.
 *
 *   tk_extract <disk.dsk> [options]
 *     --list          show the catalogue and stop
 *     --out DIR       where to write (default: the current directory)
 *     --name STEM     base name for the output pair; the default comes
 *                     from the version, matching what the NVDA add-on
 *                     and the README expect
 *     --file NAME     extract one named file verbatim instead
 *     --force         overwrite existing files
 *
 * With no options it finds the loader/OBJ pair and writes
 * <stem>.ram.bin and <stem>.obj.bin. Nothing is matched by filename --
 * see src/tk_disk.c for why, and for what is matched instead.
 *
 * DOS-order (.do, .dsk) and ProDOS-order (.po) images, and .2mg
 * wrappers around either; the ordering is detected, not assumed.
 *
 * NOTE: the library can also open a disk image directly, without this
 * step -- echotalk_create_from_disk(), and `say --disk`. This exists for
 * when you want the .bin files themselves, which is what the add-on's
 * voice discovery and every tool in tools/ still take.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsk.h"
#include "tk_disk.h"

static dsk g_disk;
static tk_disk_pair g_pair;

static int write_file(const char *dir, const char *name,
                      const uint8_t *data, size_t n, int force)
{
    char path[1024];
    FILE *f;

    if (dir && *dir) snprintf(path, sizeof path, "%s/%s", dir, name);
    else snprintf(path, sizeof path, "%s", name);

    if (!force) {
        f = fopen(path, "rb");
        if (f) {
            fclose(f);
            fprintf(stderr, "%s already exists; use --force to replace it\n",
                    path);
            return -1;
        }
    }
    f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(data, 1, n, f) != n) {
        fclose(f);
        fprintf(stderr, "short write to %s\n", path);
        return -1;
    }
    fclose(f);
    printf("%-28s %6lu bytes\n", path, (unsigned long)n);
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = NULL, *outdir = "", *stem = NULL, *one = NULL;
    int i, list = 0, force = 0;
    char err[512];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) stem = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) one = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        } else if (!image) image = argv[i];
    }
    if (!image) {
        fprintf(stderr,
                "usage: tk_extract <disk.dsk> [--list] [--out DIR] "
                "[--name STEM] [--file NAME] [--force]\n");
        return 2;
    }

    if (dsk_open(&g_disk, image, err, sizeof err) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    printf("%s: DOS 3.3, %s order%s, %d file%s\n", image,
           g_disk.prodos_order ? "ProDOS" : "DOS",
           g_disk.two_img ? " (.2mg)" : "",
           g_disk.nfiles, g_disk.nfiles == 1 ? "" : "s");

    if (list) {
        for (i = 0; i < g_disk.nfiles; i++)
            printf("  %s%-2s %4d  %s\n",
                   g_disk.file[i].locked ? "*" : " ",
                   dsk_type_name(g_disk.file[i].type),
                   g_disk.file[i].sectors, g_disk.file[i].name);
        return 0;
    }

    if (one) {
        static uint8_t buf[0x10000];
        uint16_t addr = 0;
        long n = -1;
        for (i = 0; i < g_disk.nfiles; i++) {
            if (strcmp(g_disk.file[i].name, one)) continue;
            n = dsk_read(&g_disk, &g_disk.file[i], buf, sizeof buf, &addr,
                         err, sizeof err);
            break;
        }
        if (i == g_disk.nfiles) {
            fprintf(stderr, "no file called \"%s\" on %s "
                    "(--list shows what is there)\n", one, image);
            return 1;
        }
        if (n < 0) { fprintf(stderr, "%s\n", err); return 1; }
        if (addr) printf("loads at $%04X\n", addr);
        return write_file(outdir, one, buf, (size_t)n, force) < 0 ? 1 : 0;
    }

    {
        tk_disk_match m[TK_DISK_MAXMATCH];
        int nm = tk_disk_list(&g_disk, m, TK_DISK_MAXMATCH), k;

        if (nm <= 0) {
            fprintf(stderr,
                    "no Textalker on %s: none of its %d files is a loader "
                    "carrying an entry trampoline with an engine to match\n",
                    image, g_disk.nfiles);
            fprintf(stderr, "Try --list to see what is there.\n");
            return 1;
        }

        /* Write EVERY pair. both.dsk carries v3.1.3 and v1.3 side by
         * side, and quietly extracting one of them is the shape of
         * mistake this project keeps having to unlearn. */
        for (k = 0; k < nm; k++) {
            char ram[300], obj[300], named[256];
            const char *use = stem;

            if (tk_disk_load(&g_disk, &m[k], &g_pair, err, sizeof err) < 0) {
                fprintf(stderr, "%s\n", err);
                return 1;
            }
            printf("Textalker %s: %s + %s, engine at $%04X%s\n",
                   g_pair.version,
                   g_disk.file[g_pair.loader_index].name,
                   g_disk.file[g_pair.obj_index].name,
                   g_pair.obj_addr,
                   g_pair.native_ok ? ""
                                    : "  (build unknown to the C engine; "
                                      "the emulated one runs it)");

            /* The default names are the ones README.md tells people to
             * use and the ones the NVDA add-on discovers voices by, so
             * the common case needs no --name at all. */
            if (!use) {
                /* The two names README.md documents, and the version
                 * for anything else -- a disk can carry a build that is
                 * neither, and naming every 3.x "textalker" would have
                 * one silently overwrite another. */
                if (!strcmp(g_pair.version, "3.1.3")) use = "textalker";
                else if (!strcmp(g_pair.version, "1.3")) use = "textalker_v13";
                else {
                    snprintf(named, sizeof named, "textalker_%s",
                             g_pair.version);
                    use = named;
                }
            } else if (nm > 1) {
                snprintf(named, sizeof named, "%s%d", stem, k + 1);
                use = named;                /* one --name cannot serve two */
            }
            snprintf(ram, sizeof ram, "%s.ram.bin", use);
            snprintf(obj, sizeof obj, "%s.obj.bin", use);
            if (write_file(outdir, ram, g_pair.loader, g_pair.loader_len,
                           force) < 0)
                return 1;
            if (write_file(outdir, obj, g_pair.obj, g_pair.obj_len, force) < 0)
                return 1;
        }
    }
    return 0;
}
