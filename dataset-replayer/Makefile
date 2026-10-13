CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
TARGET = build/dataset-replayer
TEST_TARGET = build/test_csv_reader

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): src/main.c src/csv_reader.c include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/csv_reader.c $(LDLIBS)

$(TEST_TARGET): tests/test_csv_reader.c src/csv_reader.c include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_csv_reader.c src/csv_reader.c $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -rf build
