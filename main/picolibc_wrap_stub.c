/* GCC 16 picolibc's setvbuf() calls __wrap___bufio_setvbuf directly
 * (baked at toolchain compile time, not via --wrap linker flag).
 * The fork IDF provides the wrapper in esp_libc, but it needs
 * __real___bufio_setvbuf which the --wrap flag is supposed to create.
 * Since --wrap doesn't help here, call __bufio_setvbuf directly. */

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

/* Forward declare the real picolibc function */
extern int __bufio_setvbuf(FILE *f, char *buf, int mode, size_t size);

int __wrap___bufio_setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    /* Simplified version: just forward to picolibc's real implementation.
     * The fork's version had an _IONBF workaround that allocates a 1-byte
     * buffer, but for initial bring-up a direct passthrough is sufficient. */
    return __bufio_setvbuf(f, buf, mode, size);
}
