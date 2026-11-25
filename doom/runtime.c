// Minimal Picolibc syscall stubs for freestanding RV32IM Doom: console output,
// bounded sbrk heap, _exit, and a read-only filesystem exposing embedded game.wad.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ecall.h"

extern unsigned char _binary_game_wad_start[];
extern unsigned char _binary_game_wad_end[];
extern char __heap_start[];
extern char __heap_end[];

static int console_put(char ch, FILE *stream) {
    (void)stream;
    rv_call(2, (unsigned char)ch);
    return 0;
}

static FILE console =
    FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);

FILE *const stdout = &console;
FILE *const stderr = &console;
FILE *const stdin = &console;

void *sbrk(int increment) {
    static char *heap_top;

    if (!heap_top)
        heap_top = __heap_start;

    if (increment < 0 ||
        (uintptr_t)increment > (uintptr_t)__heap_end - (uintptr_t)heap_top) {
        errno = ENOMEM;
        return (void *)-1;
    }

    char *prev_top = heap_top;
    heap_top += increment;
    return prev_top;
}

void _exit(int code) {
    rv_call(93, (uint32_t)code);
    for (;;) {}
}

enum { MAX_FILES = 8 };

static struct {
    int used;
    off_t position;
} open_files[MAX_FILES];

static size_t wad_size(void) {
    return (uintptr_t)_binary_game_wad_end - (uintptr_t)_binary_game_wad_start;
}

static int valid_fd(int fd) {
    if (fd >= 3 && fd < 3 + MAX_FILES && open_files[fd - 3].used)
        return 1;

    errno = EBADF;
    return 0;
}

static int is_wad(const char *path) {
    return !strcmp(path, "game.wad") || !strcmp(path, "./game.wad");
}

int open(const char *path, int flags, ...) {
    if (!is_wad(path)) {
        errno = ENOENT;
        return -1;
    }

    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
        errno = EROFS;
        return -1;
    }

    for (int i = 0; i < MAX_FILES; ++i) {
        if (!open_files[i].used) {
            open_files[i].used = 1;
            open_files[i].position = 0;
            return i + 3;
        }
    }

    errno = EMFILE;
    return -1;
}

int close(int fd) {
    if (!valid_fd(fd))
        return -1;

    open_files[fd - 3].used = 0;
    return 0;
}

ssize_t read(int fd, void *buffer, size_t count) {
    if (!valid_fd(fd))
        return -1;

    size_t pos = open_files[fd - 3].position;
    size_t remaining = pos < wad_size() ? wad_size() - pos : 0;

    if (count > remaining)
        count = remaining;

    if (count)
        memcpy(buffer, _binary_game_wad_start + pos, count);

    open_files[fd - 3].position += count;
    return count;
}

ssize_t write(int fd, const void *buffer, size_t count) {
    if (fd != 1 && fd != 2) {
        errno = EROFS;
        return -1;
    }

    const unsigned char *bytes = buffer;
    for (size_t i = 0; i < count; ++i)
        rv_call(2, bytes[i]);

    return count;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (!valid_fd(fd))
        return -1;

    int64_t base = 0;

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = open_files[fd - 3].position;
        break;
    case SEEK_END:
        base = wad_size();
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    int64_t pos = base + offset;
    if (pos < 0 || pos > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    return open_files[fd - 3].position = (off_t)pos;
}

int fstat(int fd, struct stat *stat_buf) {
    if (!valid_fd(fd))
        return -1;

    memset(stat_buf, 0, sizeof(*stat_buf));
    stat_buf->st_mode = S_IFREG | 0444;
    stat_buf->st_size = wad_size();
    return 0;
}

int stat(const char *path, struct stat *stat_buf) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    int result = fstat(fd, stat_buf);
    close(fd);
    return result;
}

int access(const char *path, int mode) {
    if (!is_wad(path)) {
        errno = ENOENT;
        return -1;
    }

    if (mode & W_OK) {
        errno = EROFS;
        return -1;
    }

    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

int remove(const char *path) {
    (void)path;
    errno = EROFS;
    return -1;
}

int rename(const char *from, const char *to) {
    (void)from;
    (void)to;
    errno = EROFS;
    return -1;
}

int system(const char *command) {
    (void)command;
    return -1;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}
