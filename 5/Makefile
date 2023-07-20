GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g

all: lib exe test

memleaks-all: lib memleaks-exe memleaks-test

lib: chat.c chat_client.c chat_server.c chat_impl.c
	gcc $(GCC_FLAGS) -c chat.c -o chat.o
	gcc $(GCC_FLAGS) -c chat_impl.c -o chat_impl.o
	gcc $(GCC_FLAGS) -c chat_client.c -o chat_client.o
	gcc $(GCC_FLAGS) -c chat_server.c -o chat_server.o

memleaks-exe: lib chat_client_exe.c chat_server_exe.c
	gcc $(GCC_FLAGS) chat_client_exe.c ../utils/heap_help/heap_help.c chat.o chat_impl.o chat_client.o -o client -ldl -rdynamic
	gcc $(GCC_FLAGS) chat_server_exe.c ../utils/heap_help/heap_help.c chat.o chat_impl.o chat_server.o -o server -ldl -rdynamic

exe: lib chat_client_exe.c chat_server_exe.c
	gcc $(GCC_FLAGS) chat_client_exe.c chat.o chat_impl.o chat_client.o -o client
	gcc $(GCC_FLAGS) chat_server_exe.c chat.o chat_impl.o chat_server.o -o server

test: lib
	gcc $(GCC_FLAGS) test.c chat.o chat_impl.o chat_client.o chat_server.o \
		-o test -I ../utils -lpthread

memleaks-test: lib
	gcc $(GCC_FLAGS) test.c ../utils/heap_help/heap_help.c chat.o chat_impl.o chat_client.o chat_server.o \
		-o test -I ../utils -lpthread -ldl -rdynamic

clean:
	rm *.o
	rm client server test
