NAME:=clox
CC:=clang
CFLAGS:=-g -O0 -Wall -std=c99 

BIN:=bin
SRC:=src
SOURCES:=$(wildcard $(SRC)/*.c)
OBJS:=$(patsubst $(SRC)/%.c, $(BIN)/%.o, $(SOURCES))

all: $(NAME)

release: CFLAGS=-Wall -O2 -DNDEBUG -std=c99
release: clean
release: $(NAME)

clean:
	@del /S /Q .\$(BIN)\*

$(NAME): $(BIN) $(OBJS)
	$(CC) -g $(OBJS) -o $(BIN)/$@.exe $(LIBS)

$(BIN)/%.o: $(SRC)/%.c
	$(CC) -c $(CFLAGS) $< -o $@

$(BIN):
	@mkdir bin
