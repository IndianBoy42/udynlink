/* Sanitizer-only regression gate for the udynlink loader.
 *
 * A normal int main() (no libFuzzer). Iterates every .bin in
 * tests/fuzz/corpus/ and runs the shared udynlink_fuzz_exercise sequence on
 * each. Asserts only that no ASan/UBSan report fires and the process exits 0;
 * it does NOT assert any particular udynlink_error_t — many corpus members
 * are real modules, others are minimal edge inputs, and all should be handled
 * gracefully.
 *
 * Build with -fsanitize=address,undefined. Run via:
 *   just test-san        # or: ctest --test-dir build-san -R udynlink_san_load
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fuzz_harness.h"

#define CORPUS_DIR "corpus"

static int has_bin_suffix(const char *name) {
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".bin") == 0;
}

static int load_and_exercise_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) {
        free(buf);
        return 0;
    }

    (void)udynlink_fuzz_exercise(buf, (size_t)sz);
    free(buf);
    return 1;
}

int main(void) {
    DIR *d = opendir(CORPUS_DIR);
    if (d == NULL) {
        fprintf(stderr, "udynlink_san_load: cannot open %s/ — "
                        "run `just fuzz-seeds` first\n", CORPUS_DIR);
        return 1;
    }

    int exercised = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!has_bin_suffix(de->d_name)) {
            continue;
        }
        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s", CORPUS_DIR, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if (load_and_exercise_file(path)) {
            exercised++;
        }
    }
    closedir(d);

    if (exercised == 0) {
        fprintf(stderr, "udynlink_san_load: no .bin files found in %s/ — "
                        "run `just fuzz-seeds` first\n", CORPUS_DIR);
        return 1;
    }

    printf("udynlink_san_load: exercised %d corpus image(s), no sanitizer "
           "reports\n", exercised);
    return 0;
}
