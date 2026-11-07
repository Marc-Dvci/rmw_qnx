/* Does memchr find the terminator when the length bound is SIZE_MAX, at every alignment of the start pointer?
 * rcutils_strndup relies on it (rcutils_strdup passes SIZE_MAX). On QNX 8.0 x86_64 libc's memchr compares the
 * length as signed in its alignment prologue, so SIZE_MAX reads as -1 and an unaligned start jumps through its table
 * at index -1. Each alignment runs in a child process, so a crash is reported instead of ending the test.
 *   qcc -Vgcc_ntox86_64 -o memchr_size_max memchr_size_max.c
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    static _Alignas(8) char buf[32];
    int bad = 0;
    for (int off = 0; off < 8; off++) {
        memset(buf, 'x', sizeof buf);
        buf[off + 7] = '\0';
        pid_t pid = fork();
        if (pid == 0) {
            volatile size_t n = SIZE_MAX; /* volatile: the compiler must not fold the call */
            const char *p = memchr(buf + off, '\0', n);
            _exit(p == buf + off + 7 ? 0 : 1);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        const char *what = WIFSIGNALED(st) ? "crashed" : WEXITSTATUS(st) == 0 ? "ok" : "wrong result";
        printf("memchr(start aligned +%d, 0, SIZE_MAX): %s\n", off, what);
        bad += strcmp(what, "ok") != 0;
    }
    return bad ? 1 : 0;
}
