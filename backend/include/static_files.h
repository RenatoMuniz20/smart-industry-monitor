#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>

/* Maior arquivo servido. O dashboard tem poucos KiB; o limite evita ler um
 * arquivo enorme colocado por engano na pasta. */
#define STATIC_MAX_SIZE (2 * 1024 * 1024)
#define STATIC_ROOT_MAX 4096

/* Arquivo carregado. `data` só existe quando o status é 200. */
typedef struct {
    char *data;
    size_t length;
    const char *content_type;  /* texto fixo, não liberar */
} StaticFile;

/* Converte `path` no caminho absoluto e canônico da pasta (realpath).
 * Retorna 0 se for um diretório; -1 caso contrário. */
int static_root_init(const char *path, char *out, size_t out_size);

/* Carrega o arquivo da `url` (já decodificada pelo servidor HTTP) de dentro de
 * `root`, que deve vir de static_root_init. Regras:
 *   - "/" vira "/index.html"; a URL precisa começar com "/".
 *   - Só letras, dígitos, ".", "_", "-" e "/"; nenhum segmento vazio nem
 *     começando com "." (bloqueia "..", "." e arquivos ocultos).
 *   - Só extensões conhecidas: html, css, js, svg, png e ico.
 *   - O caminho real (após resolver links) precisa continuar dentro de `root`.
 *   - Só arquivos regulares, até STATIC_MAX_SIZE.
 * Retorna 200 (out preenchido), 404 (não existe ou não pode ser servido) ou
 * 500 (falha de leitura ou arquivo grande demais). Libere com static_file_free. */
unsigned int static_file_load(const char *root, const char *url, StaticFile *out);

/* Libera os dados e zera a estrutura. Aceita NULL. */
void static_file_free(StaticFile *file);

#endif
