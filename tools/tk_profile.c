/*
 * tk_profile.c -- what the engine found in a Textalker image.
 *
 * src/tk_image.c no longer holds a table of addresses per release; it
 * searches each image for the shape of every table. This prints what
 * that search decided, which is the first thing to run against a build
 * nobody here has seen:
 *
 *   tk_profile <loader.bin> <obj.bin>
 *   tk_profile --disk <disk.dsk>
 *
 * A build it cannot read says so and names the table it could not find,
 * which is more use than "unrecognised". Note that a failure here means
 * only that the C REWRITE cannot run that image -- the 6502 emulation,
 * which is the default engine, runs anything.
 */
#include <stdio.h>
#include <string.h>

#include "dsk.h"
#include "tk.h"
#include "tk_disk.h"

static tk_image g_img;
static dsk g_disk;
static tk_disk_pair g_pair;

static void show(const tk_image *img)
{
    const tk_profile *p = &img->p;

    printf("Textalker %s\n", img->version);
    printf("  loads at        $%04X, %lu bytes, ends $%04X\n",
           p->base, (unsigned long)img->rom_len,
           (unsigned)(p->base + img->rom_len));
    printf("  attributes      $%04X\n", p->attrs);
    printf("  punctuation     $%04X  (names spoken by Ctrl-E A and M)\n",
           p->punct_names);
    printf("  dispatch        $%04X, rules $%04X-$%04X\n",
           p->dispatch, p->rule_lo, p->rule_hi);
    printf("  character map   $%04X  (Ctrl-V)\n", p->charmap);
    printf("  pitch offsets   $%04X   vowel variants $%04X\n",
           p->pitch_off, p->vowel_var);
    printf("  allophones      $%04X, %d of them\n", p->allo_ptr,
           p->n_allophones);
    if (p->syllabifier)
        printf("  syllabifier     prefixes $%04X suffixes $%04X "
               "digraphs $%04X clusters $%04X\n",
               p->prefixes, p->suffixes, p->digraphs, p->clusters);
    else
        printf("  syllabifier     none -- this is a 1.x\n");
    printf("  workspace       input $%04X  phonemes $%04X  flags $%04X\n",
           p->inbuf, p->phbuf, p->v_flags);
    printf("  commands        %s\n", p->commands);
    printf("  at boot         flags $%02X pitch $%02X monotone $%02X "
           "compressed $%02X\n",
           img->init_flags, img->init_base_pitch, img->init_monotone,
           img->init_compress);
}

int main(int argc, char **argv)
{
    char err[512];
    const char *pos[2] = { NULL, NULL };
    const char *disk = NULL;
    int i, npos = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disk") && i + 1 < argc) disk = argv[++i];
        else if (npos < 2) pos[npos++] = argv[i];
    }

    if (disk) {
        tk_disk_match m[TK_DISK_MAXMATCH];
        int nm, k;
        if (dsk_open(&g_disk, disk, err, sizeof err) < 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        nm = tk_disk_list(&g_disk, m, TK_DISK_MAXMATCH);
        if (nm <= 0) {
            fprintf(stderr, "no Textalker on %s\n", disk);
            return 1;
        }
        for (k = 0; k < nm; k++) {
            if (tk_disk_load(&g_disk, &m[k], &g_pair, err, sizeof err) < 0) {
                fprintf(stderr, "%s\n", err);
                return 1;
            }
            printf("%s + %s\n", g_disk.file[g_pair.loader_index].name,
                   g_disk.file[g_pair.obj_index].name);
            if (tk_image_load_mem(&g_img, g_pair.obj, g_pair.obj_len,
                                  g_pair.loader, g_pair.loader_len, NULL,
                                  err, sizeof err) < 0) {
                printf("  the C engine cannot read this build: %s\n", err);
                printf("  (the 6502 emulation still runs it)\n");
                continue;
            }
            show(&g_img);
        }
        return 0;
    }

    if (npos < 2) {
        fprintf(stderr, "usage: tk_profile <loader.bin> <obj.bin>\n"
                        "       tk_profile --disk <disk.dsk>\n");
        return 2;
    }
    if (tk_image_load(&g_img, pos[1], pos[0], NULL, err, sizeof err) < 0) {
        fprintf(stderr, "%s\n", err);
        fprintf(stderr, "(the 6502 emulation would still run it)\n");
        return 1;
    }
    show(&g_img);
    return 0;
}
