/*
 * test_text_prep.c -- unit tests for src/text_prep.c
 *
 * Pure logic, no emulator involved. Build:
 *   gcc -Wall -O2 -Isrc -o test_text_prep tools/test_text_prep.c src/text_prep.c
 */
#include <stdio.h>
#include <string.h>
#include "text_prep.h"

static int failures = 0;

/* Control characters are part of what is under test here (CR, Ctrl-E,
 * Ctrl-V), so results are escaped rather than printed raw -- a literal
 * CR in the output would otherwise scramble the report. */
static const char *esc(const char *s) {
    static char buf[4][512];
    static int which = 0;
    char *out = buf[which = (which + 1) % 4];
    char *p = out;
    for (; *s && p < out + sizeof(buf[0]) - 5; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\r')      { *p++ = '\\'; *p++ = 'r'; }
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
        else if (c < 0x20 || c >= 0x7F) {
            p += sprintf(p, "\\x%02X", c);
        }
        else *p++ = (char)c;
    }
    *p = '\0';
    return out;
}

static void check(const char *name, const char *in, const char *expect) {
    char out[512];
    size_t need = echotalk_prep_text((const uint8_t *)in, strlen(in),
                                     out, sizeof(out), NULL);
    if (strcmp(out, expect) != 0 || need != strlen(expect)) {
        printf("FAIL %-38s got \"%s\" (%zu), want \"%s\" (%zu)\n",
               name, esc(out), need, esc(expect), strlen(expect));
        failures++;
    } else {
        printf("ok   %-38s \"%s\"\n", name, esc(out));
    }
}

int main(void) {
    /* Plain ASCII must be left exactly alone -- this is what keeps the
     * existing reference_text/ inputs rendering identically. */
    check("plain ascii", "Hello, world.", "Hello, world.");
    check("ascii punctuation kept", "a-b_c/d\\e*f#g@h", "a-b_c/d\\e*f#g@h");
    check("digits kept", "1234567890", "1234567890");

    /* Line endings: the Apple II ends lines with CR, and Textalker
     * announces a stray LF as "linefeed" in all-punctuation mode. */
    check("bare LF stripped", "one\ntwo", "onetwo");
    check("CRLF collapses to CR", "one\r\ntwo", "one\rtwo");
    check("CR preserved", "one\rtwo", "one\rtwo");
    check("tab becomes space", "a\tb", "a b");
    check("other controls dropped", "a\x01\x02\x07" "b", "ab");

    /* Echo control codes must survive: Ctrl-E introduces a command,
     * Ctrl-V switches to phoneme mode. */
    check("ctrl-E survives", "\x05" "24P", "\x05" "24P");
    check("ctrl-V survives", "\x16" "OR3", "\x16" "OR3");

    /* UTF-8 input. */
    check("utf8 accented letters", "caf\xC3\xA9 na\xC3\xAFve", "cafe naive");
    check("utf8 sharp s", "Stra\xC3\x9F" "e", "Strasse");
    check("utf8 ligature", "\xC5\x93uvre", "oeuvre");
    check("utf8 curly quotes", "\xE2\x80\x9Chi\xE2\x80\x9D", "\"hi\"");
    check("utf8 apostrophe", "it\xE2\x80\x99s", "it's");
    check("utf8 em dash", "a\xE2\x80\x94" "b", "a--b");
    check("utf8 ellipsis", "wait\xE2\x80\xA6", "wait...");
    check("utf8 nbsp", "a\xC2\xA0" "b", "a b");
    check("utf8 degrees", "20\xC2\xB0", "20 degrees");
    check("utf8 euro", "5\xE2\x82\xAC", "5 euros");
    check("utf8 zero width dropped", "a\xE2\x80\x8B" "b", "ab");
    check("utf8 BOM dropped", "\xEF\xBB\xBFhi", "hi");
    check("utf8 latin ext-A", "\xC5\xBE" "ivot", "zivot");

    /* Windows-1252 / Latin-1 input, i.e. bytes that are not valid UTF-8.
     * These must not be mistaken for UTF-8 or passed through raw -- a
     * raw high byte would collide with the high-bit transport
     * convention and be spoken as something arbitrary. */
    check("cp1252 curly quote", "it\x92s", "it's");
    check("cp1252 em dash", "a\x97" "b", "a--b");
    check("cp1252 ellipsis", "wait\x85", "wait...");
    check("latin1 e acute", "caf\xE9", "cafe");
    check("latin1 sharp s", "Stra\xDF" "e", "Strasse");
    check("cp1252 euro", "5\x80", "5 euros");

    /* Mixed encodings in one string: real-world input is not always
     * self-consistent, and per-character detection handles it. */
    check("mixed utf8 and cp1252", "caf\xC3\xA9 and caf\xE9", "cafe and cafe");

    /* Unmapped characters fall back to a space by default, so words
     * cannot silently run together. */
    check("unmapped becomes space", "a\xE4\xB8\xAD" "b", "a b");

    /* Nothing here should ever emit a byte with the high bit set. */
    {
        const char *stress = "\xC3\xA9\x92\x85\xE2\x82\xAC\xE4\xB8\xAD\xDF";
        char out[256];
        echotalk_prep_text((const uint8_t *)stress, strlen(stress),
                           out, sizeof(out), NULL);
        int clean = 1;
        for (char *p = out; *p; p++)
            if ((unsigned char)*p & 0x80) clean = 0;
        printf("%s   %-38s\n", clean ? "ok  " : "FAIL", "output is always 7-bit");
        if (!clean) failures++;
    }

    /* Dropping instead of substituting, and the measure-then-fill
     * calling pattern. */
    {
        echotalk_prep_opts o;
        echotalk_prep_defaults(&o);
        o.unmapped = 0;
        char out[64];
        echotalk_prep_text((const uint8_t *)"a\xE4\xB8\xAD" "b", 5, out, sizeof(out), &o);
        int ok = !strcmp(out, "ab");
        printf("%s   %-38s \"%s\"\n", ok ? "ok  " : "FAIL", "unmapped dropped when requested", out);
        if (!ok) failures++;

        size_t need = echotalk_prep_text((const uint8_t *)"20\xC2\xB0", 4, NULL, 0, NULL);
        ok = (need == strlen("20 degrees"));
        printf("%s   %-38s %zu\n", ok ? "ok  " : "FAIL", "size query with NULL buffer", need);
        if (!ok) failures++;
    }

    /* Truncation must report the full required length, snprintf-style,
     * and must not overrun. */
    {
        char small[5];
        size_t need = echotalk_prep_text((const uint8_t *)"abcdefgh", 8,
                                         small, sizeof(small), NULL);
        int ok = (need == 8) && !strcmp(small, "abcd");
        printf("%s   %-38s \"%s\" need=%zu\n", ok ? "ok  " : "FAIL",
               "truncation reports full length", small, need);
        if (!ok) failures++;
    }

    /* A truncated UTF-8 sequence at the end of the buffer must not read
     * past the end. It is indistinguishable from a legitimate Latin-1
     * character, so it takes the documented Windows-1252 fallback --
     * $C3 becomes A-tilde, hence "A". The point of the test is that it
     * terminates safely and produces ASCII, not the specific letter. */
    check("truncated utf8 tail is safe", "ab\xC3", "abA");

    printf("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
