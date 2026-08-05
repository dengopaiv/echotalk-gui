#include <stdio.h>
#include <string.h>
#include "chunker.h"

static void run_test(const char *label, const char *text, size_t max_size) {
    printf("=== %s (max_chunk_size=%zu) ===\n", label, max_size);
    printf("input: \"%s\"\n", text);
    size_t len = strlen(text);
    echotalk_chunk chunks[64];
    size_t n = echotalk_chunk_text(text, len, max_size, chunks, 64);
    for (size_t i = 0; i < n; i++) {
        printf("  chunk %zu (%zu bytes): \"%.*s\"\n",
               i, chunks[i].length, (int)chunks[i].length, text + chunks[i].offset);
        if (chunks[i].length > max_size) {
            printf("  !!! OVERFLOW: chunk exceeds max_chunk_size !!!\n");
        }
    }
    printf("\n");
}

int main(void) {
    run_test("short text, fits in one chunk",
              "Hello, world!", 80);

    run_test("clause-boundary splitting",
              "This is a longer sentence, which has several clauses, and should split "
              "at commas or periods rather than in awkward places whenever possible.",
              40);

    run_test("no clause boundary, falls back to word boundary",
              "the quick brown fox jumps over the lazy dog again and again without any punctuation at all",
              30);

    run_test("keysmash: no clause boundary or whitespace, hard split required",
              "asdkfjhaslkdfjhalskdjfhalskdjfhalskdjfhalskdjfhalskdjfhalskdjfh",
              20);

    run_test("mixed: normal words then a long unbroken token then more words",
              "check this out httpsxxxxreallyreallyreallyreallylongurlwithnobreaksatallxxxx okay thanks",
              25);

    run_test("empty input",
              "", 40);

    run_test("exactly at boundary",
              "12345678901234567890", 20);

    run_test("colon and semicolon as clause boundaries",
              "Ingredients: flour; sugar; eggs; butter. Mix well and bake at 350 degrees for twenty five minutes.",
              35);

    return 0;
}
