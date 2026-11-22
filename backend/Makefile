CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude -I/usr/include/json-c
LDLIBS = -lmicrohttpd -lcurl -ljson-c

BUILD = build
BIN = $(BUILD)/backend
TEST_QUERY = $(BUILD)/test_interscity_query
TEST_STATIC = $(BUILD)/test_static_files
SRCS = src/main.c src/http_server.c src/routes.c src/machine_map.c src/interscity_query.c \
       src/static_files.c
QUERY_SRCS = src/interscity_query.c
HDRS = include/http_server.h include/routes.h include/machine_map.h include/interscity_query.h \
       include/static_files.h

.PHONY: all test smoke-real clean

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

# Sem bibliotecas externas: só o módulo de arquivos estáticos (SIM-07).
$(TEST_STATIC): tests/test_static_files.c src/static_files.c include/static_files.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_static_files.c src/static_files.c

$(TEST_QUERY): tests/test_interscity_query.c $(QUERY_SRCS) include/interscity_query.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_interscity_query.c $(QUERY_SRCS) $(LDLIBS)

test: $(BIN) $(TEST_QUERY) $(TEST_STATIC)
	python3 -B tests/test_backend.py $(BIN)
	python3 -B tests/test_interscity_query.py $(TEST_QUERY)
	python3 -B tests/test_machines_latest.py $(BIN)
	python3 -B tests/test_machines_history.py $(BIN)
	./$(TEST_STATIC)
	python3 -B tests/test_dashboard_static.py $(BIN)

# Somente leitura, contra a instância real da UFMA. Não entra no `test`.
smoke-real: $(BIN)
	python3 -B tests/smoke_real_interscity.py $(BIN) ../config/interscity/machines.conf https://cidadesinteligentes.lsdi.ufma.br/interscity_lh

clean:
	rm -rf $(BUILD)
