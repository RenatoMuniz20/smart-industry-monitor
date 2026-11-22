/* Testes do módulo de arquivos estáticos (SIM-07). Não usa rede. */
#define _XOPEN_SOURCE 700

#include "static_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, label)                      \
    do {                                        \
        if (cond) {                             \
            printf("PASS: %s\n", label);        \
        } else {                                \
            printf("FAIL: %s\n", label);        \
            failures++;                         \
        }                                       \
    } while (0)

static void write_file(const char *dir, const char *name, const char *content)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

/* Status esperado para a URL; quando 200, confere tipo e conteúdo. */
static int load_is(const char *root, const char *url, unsigned int status,
                   const char *type, const char *content)
{
    StaticFile file;
    unsigned int got = static_file_load(root, url, &file);
    int ok = got == status;

    if (ok && status == 200) {
        ok = file.content_type != NULL && strcmp(file.content_type, type) == 0 &&
             file.length == strlen(content) && memcmp(file.data, content, file.length) == 0;
    }
    if (ok && status != 200) {
        ok = file.data == NULL && file.length == 0;
    }
    static_file_free(&file);
    return ok;
}

int main(void)
{
    char base[] = "/tmp/sim07-static-XXXXXX";
    char web[512], outside[512], root[STATIC_ROOT_MAX], path[1024];

    if (mkdtemp(base) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(web, sizeof web, "%s/web", base);
    snprintf(outside, sizeof outside, "%s/outside", base);
    mkdir(web, 0755);
    mkdir(outside, 0755);
    snprintf(path, sizeof path, "%s/sub", web);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/pasta.js", web);
    mkdir(path, 0755);

    write_file(web, "index.html", "<!doctype html><p>ok</p>");
    write_file(web, "app.js", "console.log(1);");
    write_file(web, "style.css", "body{}");
    write_file(web, "sub/extra.css", "p{}");
    write_file(web, ".oculto.js", "secret");
    write_file(web, "notas.txt", "texto");
    write_file(web, "vazio.js", "");
    write_file(outside, "segredo.js", "secret");

    snprintf(path, sizeof path, "%s/link-fora.js", web);
    if (symlink("../outside/segredo.js", path) != 0) perror("symlink");
    snprintf(path, sizeof path, "%s/link-dentro.js", web);
    if (symlink("app.js", path) != 0) perror("symlink");

    /* Arquivo maior que o limite. */
    snprintf(path, sizeof path, "%s/grande.js", web);
    {
        FILE *f = fopen(path, "w");
        if (f == NULL || fseek(f, STATIC_MAX_SIZE, SEEK_SET) != 0 || fputc('x', f) == EOF) {
            perror("grande.js");
            return 2;
        }
        fclose(f);
    }

    CHECK(static_root_init(web, root, sizeof root) == 0, "pasta do dashboard aceita");
    snprintf(path, sizeof path, "%s/index.html", web);
    CHECK(static_root_init(path, root + 0, 0) == -1, "tamanho de saída zero é recusado");
    {
        char other[STATIC_ROOT_MAX];
        CHECK(static_root_init(path, other, sizeof other) == -1, "arquivo no lugar da pasta é recusado");
        CHECK(static_root_init("/nao/existe/sim07", other, sizeof other) == -1,
              "pasta inexistente é recusada");
    }

    CHECK(load_is(root, "/", 200, "text/html; charset=utf-8", "<!doctype html><p>ok</p>"),
          "/ entrega index.html como HTML");
    CHECK(load_is(root, "/index.html", 200, "text/html; charset=utf-8", "<!doctype html><p>ok</p>"),
          "/index.html entregue");
    CHECK(load_is(root, "/app.js", 200, "text/javascript; charset=utf-8", "console.log(1);"),
          "JavaScript com o tipo correto");
    CHECK(load_is(root, "/style.css", 200, "text/css; charset=utf-8", "body{}"),
          "CSS com o tipo correto");
    CHECK(load_is(root, "/sub/extra.css", 200, "text/css; charset=utf-8", "p{}"),
          "arquivo em subpasta entregue");
    CHECK(load_is(root, "/vazio.js", 200, "text/javascript; charset=utf-8", ""),
          "arquivo vazio entregue com tamanho zero");
    CHECK(load_is(root, "/link-dentro.js", 200, "text/javascript; charset=utf-8", "console.log(1);"),
          "link para arquivo dentro da pasta é aceito");

    CHECK(load_is(root, "/../outside/segredo.js", 404, NULL, NULL), "../ para fora da pasta: 404");
    CHECK(load_is(root, "/sub/../app.js", 404, NULL, NULL), "segmento .. no meio: 404");
    CHECK(load_is(root, "/./app.js", 404, NULL, NULL), "segmento . : 404");
    CHECK(load_is(root, "/link-fora.js", 404, NULL, NULL), "link para fora da pasta: 404");
    CHECK(load_is(root, "/.oculto.js", 404, NULL, NULL), "arquivo oculto: 404");
    CHECK(load_is(root, "/notas.txt", 404, NULL, NULL), "extensão fora da lista: 404");
    CHECK(load_is(root, "/sub", 404, NULL, NULL), "pasta sem extensão: 404");
    CHECK(load_is(root, "/sub/", 404, NULL, NULL), "barra no final: 404");
    CHECK(load_is(root, "/pasta.js", 404, NULL, NULL), "pasta com nome .js: 404");
    CHECK(load_is(root, "/nao-existe.js", 404, NULL, NULL), "arquivo inexistente: 404");
    CHECK(load_is(root, "//app.js", 404, NULL, NULL), "barra dupla: 404");
    CHECK(load_is(root, "app.js", 404, NULL, NULL), "URL sem / inicial: 404");
    CHECK(load_is(root, "/app%2ejs", 404, NULL, NULL), "caractere % : 404");
    CHECK(load_is(root, "/app.js\n", 404, NULL, NULL), "caractere de controle: 404");
    CHECK(load_is(root, "/app.JS", 404, NULL, NULL), "extensão em maiúsculas: 404");
    {
        char long_url[400];
        memset(long_url, 'a', sizeof long_url);
        long_url[0] = '/';
        memcpy(long_url + sizeof long_url - 4, ".js", 4);
        CHECK(load_is(root, long_url, 404, NULL, NULL), "URL longa demais: 404");
    }
    CHECK(load_is(root, "/grande.js", 500, NULL, NULL), "arquivo acima do limite: 500");
    CHECK(load_is(NULL, "/app.js", 404, NULL, NULL), "sem pasta configurada: 404");
    CHECK(static_file_load(root, "/app.js", NULL) == 500, "saída nula: 500");
    static_file_free(NULL);

    printf("%s\n", failures == 0 ? "Todos os testes de static_files passaram."
                                 : "Há falhas em static_files.");
    {
        char command[1200];
        snprintf(command, sizeof command, "rm -rf '%s'", base);
        if (system(command) != 0) fprintf(stderr, "aviso: não removeu %s\n", base);
    }
    return failures == 0 ? 0 : 1;
}
