#include "chunker.h"

static int is_clause_boundary(char c) {
    switch (c) {
        case ',': case '.': case '?': case '!': case ';': case ':':
            return 1;
        default:
            return 0;
    }
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Skip leading whitespace, returning the new start offset. */
static size_t skip_ws(const char *text, size_t pos, size_t text_len) {
    while (pos < text_len && is_space(text[pos])) pos++;
    return pos;
}

size_t echotalk_chunk_text(const char *text, size_t text_len,
                            size_t max_chunk_size,
                            echotalk_chunk *chunks, size_t max_chunks) {
    if (text_len == 0 || max_chunk_size == 0) return 0;

    size_t pos = skip_ws(text, 0, text_len);
    size_t count = 0;

    while (pos < text_len) {
        if (count >= max_chunks) break;

        size_t remaining = text_len - pos;

        if (remaining <= max_chunk_size) {
            /* Last chunk: trim trailing whitespace only. */
            size_t end = text_len;
            while (end > pos && is_space(text[end - 1])) end--;
            if (end > pos) {
                chunks[count].offset = pos;
                chunks[count].length = end - pos;
                count++;
            }
            pos = text_len;
            break;
        }

        size_t window_end = pos + max_chunk_size; /* exclusive */

        /* 1. Look for the last clause-boundary char in the window. */
        size_t split_at = 0; /* 0 = "not found" sentinel (pos can't be 0 here since we'd have taken the "last chunk" branch) */
        for (size_t i = window_end; i > pos; i--) {
            if (is_clause_boundary(text[i - 1])) {
                split_at = i; /* split AFTER this char (include it in the chunk) */
                break;
            }
        }

        if (split_at != 0) {
            chunks[count].offset = pos;
            chunks[count].length = split_at - pos;
            count++;
            pos = skip_ws(text, split_at, text_len);
            continue;
        }

        /* 2. No clause boundary -- look for the last whitespace run in the window. */
        size_t ws_at = 0;
        for (size_t i = window_end; i > pos; i--) {
            if (is_space(text[i - 1])) {
                ws_at = i - 1; /* split BEFORE the whitespace */
                break;
            }
        }

        if (ws_at != 0) {
            chunks[count].offset = pos;
            chunks[count].length = ws_at - pos;
            count++;
            pos = skip_ws(text, ws_at, text_len);
            continue;
        }

        /* 3. No clause boundary, no whitespace at all in the window --
         * a pathologically long "word" (keysmash, URL, etc). Hard-split
         * at exactly max_chunk_size. */
        chunks[count].offset = pos;
        chunks[count].length = max_chunk_size;
        count++;
        pos = window_end; /* no whitespace to skip -- we cut mid-"word" */
    }

    return count;
}
