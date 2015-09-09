# libevent-http-server
A simple http server using libevent

## How to run
Compilation:
	gcc -o server server.c -levent

Running:
	./server [port, defaults to 8080]

## Storing files
This server will look for `htdocs` folder in the same directory as the executable. This server accepts html files only.
