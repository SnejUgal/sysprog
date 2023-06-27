GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant
FILES = command.c parser.c solution.c

all: $(FILES)
	gcc $(GCC_FLAGS) $(FILES)

prompt: $(FILES)
	gcc $(GCC_FLAGS) -D PROMPT $(FILES)

memleaks: $(FILES)
	gcc $(GCC_FLAGS) -ldl -rdynamic ../utils/heap_help/heap_help.c $(FILES)

clean:
	rm a.out
