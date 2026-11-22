#define _XOPEN_SOURCE 700

#include "static_files.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define URL_MAX 256

static const struct {
    const char *extension;
    const char *content_type;
} TYPES[] = {
    {".html", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".ico", "image/x-icon"},
};

int static_root_init(const char *path, char *out, size_t out_size)
{
    char resolved[PATH_MAX];
    struct stat st;

    if (path == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    if (realpath(path, resolved) == NULL || stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    if (strlen(resolved) + 1 > out_size) {
        return -1;
    }
    memcpy(out, resolved, strlen(resolved) + 1);
    return 0;
}

/* Valida a forma da URL antes de tocar no sistema de arquivos. */
static int url_is_safe(const char *url)
{
    size_t length = strlen(url);

    if (length < 2 || length > URL_MAX || url[0] != '/') {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        char c = url[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) {
            return 0;
        }
        /* Cada segmento começa depois de "/": não pode ser vazio nem começar com ".". */
        if (c == '/' && (i + 1 == length || url[i + 1] == '/' || url[i + 1] == '.')) {
            return 0;
        }
    }
    return 1;
}

static const char *content_type_of(const char *url)
{
    const char *dot = strrchr(url, '.');
    const char *slash = strrchr(url, '/');

    if (dot == NULL || dot < slash) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
        if (strcmp(dot, TYPES[i].extension) == 0) {
            return TYPES[i].content_type;
        }
    }
    return NULL;
}

unsigned int static_file_load(const char *root, const char *url, StaticFile *out)
{
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    size_t root_length;
    struct stat st;
    const char *type;
    FILE *file;
    char *data;

    if (out == NULL) {
        return 500;
    }
    memset(out, 0, sizeof *out);
    if (root == NULL || url == NULL) {
        return 404;
    }
    if (strcmp(url, "/") == 0) {
        url = "/index.html";
    }
    if (!url_is_safe(url) || (type = content_type_of(url)) == NULL) {
        return 404;
    }

    root_length = strlen(root);
    if (root_length + strlen(url) + 1 > sizeof candidate) {
        return 404;
    }
    memcpy(candidate, root, root_length);
    memcpy(candidate + root_length, url, strlen(url) + 1);

    /* O caminho real precisa continuar dentro da pasta, mesmo com links. */
    if (realpath(candidate, resolved) == NULL ||
        strncmp(resolved, root, root_length) != 0 || resolved[root_length] != '/') {
        return 404;
    }
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 404;
    }
    if (st.st_size < 0 || st.st_size > STATIC_MAX_SIZE) {
        return 500;
    }

    file = fopen(resolved, "rb");
    if (file == NULL) {
        return 404;
    }
    /* +1 evita malloc(0) para arquivo vazio. */
    data = malloc((size_t)st.st_size + 1);
    if (data == NULL) {
        fclose(file);
        return 500;
    }
    if (fread(data, 1, (size_t)st.st_size, file) != (size_t)st.st_size) {
        free(data);
        fclose(file);
        return 500;
    }
    fclose(file);

    out->data = data;
    out->length = (size_t)st.st_size;
    out->content_type = type;
    return 200;
}

void static_file_free(StaticFile *file)
{
    if (file == NULL) {
        return;
    }
    free(file->data);
    memset(file, 0, sizeof *file);
}
