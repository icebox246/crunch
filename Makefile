server: server.c
	gcc $^ -o $@ `pkg-config libmd x11 --cflags --libs` -fsanitize=undefined
