/* Picolibc host hooks: a read-only WAD, a bounded heap, and console output.
 * No guest path ever opens a file on the host machine. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ecall.h"

extern unsigned char _binary_game_wad_start[], _binary_game_wad_end[];
extern char __heap_start[], __heap_end[];

static int console_put(char c, FILE *stream) {
    (void)stream;
    rv_call(2, (unsigned char)c);
    return 0;
}
static FILE console = FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);
FILE *const stdout = &console;
FILE *const stderr = &console;
FILE *const stdin = &console;

void *sbrk(int increment) {
    static char *top;
    if (!top) top = __heap_start;
    if (increment < 0 || (uintptr_t)increment > (uintptr_t)__heap_end - (uintptr_t)top) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *old = top;
    top += increment;
    return old;
}

void _exit(int code) {
    rv_call(93, (uint32_t)code);
    for (;;) {}
}

enum { MAX_FILES = 8 };
static struct { int used; off_t position; } files[MAX_FILES];
static size_t wad_size(void) {
    return (uintptr_t)_binary_game_wad_end - (uintptr_t)_binary_game_wad_start;
}
static int valid_fd(int fd) {
    if (fd >= 3 && fd < 3 + MAX_FILES && files[fd - 3].used) return 1;
    errno = EBADF;
    return 0;
}
static int is_wad(const char *path) {
    return !strcmp(path, "game.wad") || !strcmp(path, "./game.wad");
}
int open(const char *path, int flags, ...) {
    if (!is_wad(path)) { errno = ENOENT; return -1; }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
        errno = EROFS; return -1;
    }
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!files[i].used) {
            files[i].used = 1;
            files[i].position = 0;
            return i + 3;
        }
    }
    errno = EMFILE;
    return -1;
}
int close(int fd) {
    if (!valid_fd(fd)) return -1;
    files[fd - 3].used = 0;
    return 0;
}
ssize_t read(int fd, void *buffer, size_t count) {
    if (!valid_fd(fd)) return -1;
    size_t pos = files[fd - 3].position;
    size_t remaining = pos < wad_size() ? wad_size() - pos : 0;
    if (count > remaining) count = remaining;
    if (count) memcpy(buffer, _binary_game_wad_start + pos, count);
    files[fd - 3].position += count;
    return count;
}
ssize_t write(int fd, const void *buffer, size_t count) {
    if (fd != 1 && fd != 2) { errno = EROFS; return -1; }
    const unsigned char *p = buffer;
    for (size_t i = 0; i < count; ++i) rv_call(2, p[i]);
    return count;
}
off_t lseek(int fd, off_t offset, int whence) {
    if (!valid_fd(fd)) return -1;
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = files[fd - 3].position; break;
    case SEEK_END: base = wad_size(); break;
    default: errno = EINVAL; return -1;
    }
    int64_t pos = base + offset;
    if (pos < 0 || pos > INT32_MAX) { errno = EINVAL; return -1; }
    return files[fd - 3].position = (off_t)pos;
}
int fstat(int fd, struct stat *st) {
    if (!valid_fd(fd)) return -1;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = wad_size();
    return 0;
}
int stat(const char *path, struct stat *st) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int result = fstat(fd, st);
    close(fd);
    return result;
}
int access(const char *path, int mode) {
    if (!is_wad(path)) { errno = ENOENT; return -1; }
    if (mode & W_OK) { errno = EROFS; return -1; }
    return 0;
}
int mkdir(const char *path, mode_t mode) {
    (void)path; (void)mode;
    errno = EROFS;
    return -1;
}
int remove(const char *path) { (void)path; errno = EROFS; return -1; }
int rename(const char *from, const char *to) {
    (void)from; (void)to; errno = EROFS; return -1;
}
int system(const char *command) { (void)command; return -1; }
char *getenv(const char *name) { (void)name; return NULL; }
