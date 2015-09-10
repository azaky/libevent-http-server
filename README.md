# libevent-http-server
A simple http server using libevent. More info about libevent, visit [http://libevent.org/](http://libevent.org/)

## How to run
Compilation:

	gcc -o server-evhttp server-evhttp.c -levent

Running:

	./server-evhttp [port, defaults to 8080]

## Storing files
This server will look for `htdocs` folder in the same directory as the executable. This server accepts html files only.
