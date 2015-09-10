#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 8080

char uri_root[512];
char *doc_root = "htdocs";

static const struct table_entry {
	const char *extension;
	const char *content_type;
} content_type_table[] = {
	{ "txt", "text/plain" },
	{ "c", "text/plain" },
	{ "h", "text/plain" },
	{ "html", "text/html" },
	{ "htm", "text/htm" },
	{ "css", "text/css" },
	{ "gif", "image/gif" },
	{ "jpg", "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "png", "image/png" },
	{ "pdf", "application/pdf" },
	{ "ps", "application/postsript" },
	{ NULL, NULL },
};

/* Try to guess a good content-type for 'path' */
static const char * guess_content_type(const char *path)
{
	const char *last_period, *extension;
	const struct table_entry *ent;
	last_period = strrchr(path, '.');
	if (!last_period || strchr(last_period, '/'))
		goto not_found; /* no exension */
	extension = last_period + 1;
	for (ent = &content_type_table[0]; ent->extension; ++ent) {
		if (!evutil_ascii_strcasecmp(ent->extension, extension))
			return ent->content_type;
	}

not_found:
	return "application/misc";
}

static int encode_request_to_file_path(const char* uri, char** whole_path) {
	const char *path;
	char *decoded_path;
	struct evhttp_uri *decoded = NULL;
	struct stat st;
	size_t len;

	*whole_path = NULL;

	decoded = evhttp_uri_parse(uri);
	if (!decoded) {
		printf("It's not a good URI. Sending 404.\n");
		return 0;
	}

	/* Let's see what path the user asked for. */
	path = evhttp_uri_get_path(decoded);
	if (!path) {
		path = "/";
	}

	/* We need to decode it, to see what path the user really wanted. */
	decoded_path = evhttp_uridecode(path, 0, NULL);
	if (decoded_path == NULL) {
		return 0;
	}

	len = strlen(decoded_path) + strlen(doc_root) + 15;
	*whole_path = (char*)malloc(len);
	if (!*whole_path) {
		perror("malloc");
		evhttp_uri_free(decoded);
		free(decoded_path);
		return 0;
	}
	evutil_snprintf(*whole_path, len, "%s%s", doc_root, decoded_path);

	if (stat(*whole_path, &st) < 0) {
		evhttp_uri_free(decoded);
		free(decoded_path);
		return 0;
	}

	if (S_ISDIR(st.st_mode)) {
		// Find index.html of this page
		if (*whole_path[strlen(*whole_path) - 1] == '/') {
			strcat(*whole_path, "index.html");
		} else {
			strcat(*whole_path, "/index.html");
		}
		if (stat(*whole_path, &st) < 0) {
			evhttp_uri_free(decoded);
			free(decoded_path);
			return 0;
		}
	}
	evhttp_uri_free(decoded);
	free(decoded_path);
	return st.st_size;
}

static void send_document_cb(struct evhttp_request *req, void *arg) {
	struct evbuffer *evb = NULL;
	const char *uri = evhttp_request_get_uri(req);
	char *whole_path = NULL;
	int fd = -1, fsize;

	printf("Got a GET request for <%s>\n",  uri);

	fsize = encode_request_to_file_path(uri, &whole_path);
	if (!fsize) {
		goto err;
	}

	evb = evbuffer_new();

	const char *type = guess_content_type(whole_path);
	if ((fd = open(whole_path, O_RDONLY)) < 0) {
		perror("open");
		goto err;
	}

	evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", type);
	evbuffer_add_file(evb, fd, 0, fsize);

	evhttp_send_reply(req, 200, "OK", evb);
	goto done;
err:
	evhttp_send_error(req, 404, "Document was not found");
	if (fd>=0) {
		close(fd);
	}
done:
	if (whole_path) {
		free(whole_path);
	}
	if (evb) {
		evbuffer_free(evb);
	}
}

void diplay_socket_information(struct evhttp_bound_socket *handle) {
	struct sockaddr_storage ss;
	evutil_socket_t fd;
	ev_socklen_t socklen = sizeof(ss);
	char addrbuf[128];
	void *inaddr;
	const char *addr;
	int got_port = -1;

	fd = evhttp_bound_socket_get_fd(handle);
	memset(&ss, 0, sizeof(ss));
	if (getsockname(fd, (struct sockaddr *)&ss, &socklen)) {
		perror("getsockname() failed");
		exit(1);
	}
	if (ss.ss_family == AF_INET) {
		got_port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
		inaddr = &((struct sockaddr_in*)&ss)->sin_addr;
	} else {
		fprintf(stderr, "Weird address family %d\n", ss.ss_family);
		exit(1);
	}
	addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf, sizeof(addrbuf));
	if (addr) {
		printf("Listening on %s:%d\n", addr, got_port);
		evutil_snprintf(uri_root, sizeof(uri_root),
			"http://%s:%d",addr,got_port);
	} else {
		fprintf(stderr, "evutil_inet_ntop failed\n");
		exit(1);
	}
}

int main(int argc, char **argv) {
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *handle;
	unsigned short port = DEFAULT_PORT;

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		return 1;
	}

	if (argc >= 2) {
		port = atoi(argv[1]);
		if (!port) {
			port = DEFAULT_PORT;
		}
	}

	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		return 1;
	}

	http = evhttp_new(base);
	if (!http) {
		fprintf(stderr, "couldn't create evhttp. Exiting.\n");
		return 1;
	}

	evhttp_set_gencb(http, send_document_cb, argv[1]);

	handle = evhttp_bind_socket_with_handle(http, "127.0.0.1", port);
	if (!handle) {
		fprintf(stderr, "couldn't bind to port %d. Exiting.\n",
			(int)port);
		return 1;
	}

	diplay_socket_information(handle);

	event_base_dispatch(base);

	return 0;
}
