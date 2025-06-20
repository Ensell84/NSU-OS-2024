#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <termios.h>
#include <sys/select.h>

#define LINES_PER_PAGE 25
#define HTTP_BUFFER_SIZE (64 * 1024)

typedef struct {
	char* buffer;
	int size;
	int head;
	int tail;
} ring_buffer_t;

static struct termios orig_term;

void set_non_canonical() {
	tcgetattr(STDIN_FILENO, &orig_term);

	struct termios new_term = orig_term;
	new_term.c_lflag &= ~(ICANON | ECHO);
	new_term.c_cc[VMIN] = 1;
	new_term.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
}

void restore_canonical() {
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

void ring_buffer_init(ring_buffer_t *rb, char *buffer, int size) {
	rb->buffer = buffer;
	rb->size = size;
	rb->head = 0;
	rb->tail = 0;
}

int ring_buffer_used_space(ring_buffer_t *rb) {
	int used = rb->head - rb->tail;
	if (used < 0) used += rb->size;
	return used;
}

int ring_buffer_free_space(ring_buffer_t *rb) {
	return rb->size - ring_buffer_used_space(rb) - 1;
}

int add_data_to_buffer(int sock, ring_buffer_t *rb) {
	char temp_buffer[BUFSIZ];

	int free_space = ring_buffer_free_space(rb);
	if (free_space <= 0) return 0;

	int read_size = (free_space < sizeof(temp_buffer)) ? free_space : sizeof(temp_buffer);
	int bytes = read(sock, temp_buffer, read_size);

	if (bytes <= 0) return bytes;

	int bytes_to_end = rb->size - rb->head;
	if (bytes <= bytes_to_end) {
		memcpy(rb->buffer + rb->head, temp_buffer, bytes);
		rb->head += bytes;

		if (rb->head == rb->size) rb->head = 0;
	} else {
		memcpy(rb->buffer + rb->head, temp_buffer, bytes_to_end);
		memcpy(rb->buffer, temp_buffer + bytes_to_end, bytes - bytes_to_end);

		rb->head = bytes - bytes_to_end;
	}

	return bytes;
}

int display_page(ring_buffer_t *rb, int interactive, int connection_closed) {
	int lines_counted = 0;
	int temp_tail = rb->tail;

	while (temp_tail != rb->head && lines_counted < LINES_PER_PAGE) {
		if (rb->buffer[temp_tail] == '\n') lines_counted++;
		temp_tail++;
		if (temp_tail >= rb->size) temp_tail = 0;
	}

	if (lines_counted < LINES_PER_PAGE && !connection_closed) {
		return -1;
	}

	int lines_shown = 0;
	char last_printed = 0;
	int printed_any = 0;
	int start_tail = rb->tail;
	int used = ring_buffer_used_space(rb);

	if (used == 0) return 0;

	while (rb->tail != rb->head && lines_shown < LINES_PER_PAGE) {
		char ch = rb->buffer[rb->tail];
		rb->tail++;
		if (rb->tail >= rb->size) rb->tail = 0;

		if (ch == '\n') lines_shown++;

		putchar(ch);
		last_printed = ch;
		printed_any = 1;
	}

	int bytes_processed = rb->tail - start_tail;
	if (bytes_processed < 0) bytes_processed += rb->size;

	if (interactive && (rb->tail != rb->head || !connection_closed)) {
		if (printed_any && last_printed != '\n') {
			putchar('\n');
		}
		printf("Press space to scroll down...\n");
	}

	return bytes_processed;
}

int send_http_request(int sock, const char *host, const char *path, size_t host_len, size_t path_len) {
	char request[host_len + path_len + 50];
	int len = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

	if (write(sock, request, len) < 0) {
		perror("write");
		return -1;
	}
	return 0;
}

int create_socket(const char *host, uint16_t port) {
	struct hostent *server;
	struct sockaddr_in server_addr;
	int sock;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		return -1;
	}

	server = gethostbyname(host);
	if (server == NULL) {
		fprintf(stderr, "Error: No such host\n");
		close(sock);
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
	server_addr.sin_port = htons(port);

	if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
		perror("connect");
		close(sock);
		return -1;
	}

	return sock;
}

void parse_url(const char *url, size_t *host_len, size_t *path_len,
		const char **host_start, const char **path_start, uint16_t *port) {
	const char *protocol_end = strstr(url, "://");
	const char *hostname_begin = protocol_end ? protocol_end + 3 : url;
	const char *path_delimiter = strchr(hostname_begin, '/');
	const char *port_delimiter = strchr(hostname_begin, ':');

	if (port_delimiter && path_delimiter && port_delimiter > path_delimiter) {
		port_delimiter = NULL;
	}

	if (port_delimiter) {
		int parsed_port = atoi(port_delimiter + 1);
		if (parsed_port > 0 && parsed_port <= 65535) {
			*port = parsed_port;
		}
	}

	*host_start = hostname_begin;

	if (path_delimiter) {
		*host_len = port_delimiter ? (port_delimiter - hostname_begin) : (path_delimiter - hostname_begin);
		*path_start = path_delimiter;
		*path_len = strlen(path_delimiter);
	} else {
		*host_len = port_delimiter ? (port_delimiter - hostname_begin) : strlen(hostname_begin);
		*path_start = "/";
		*path_len = 1;
	}
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <URL>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	const char *url = argv[1];
	int interactive = isatty(STDOUT_FILENO);
	uint16_t port = 80;

	size_t host_len, path_len;
	const char *host_start, *path_start;

	parse_url(url, &host_len, &path_len, &host_start, &path_start, &port);
	char host[host_len + 1];
	char path[path_len + 1];

	strncpy(host, host_start, host_len);
	host[host_len] = '\0';
	strncpy(path, path_start, path_len);
	path[path_len] = '\0';

	int sock = create_socket(host, port);
	if (sock < 0) {
		exit(EXIT_FAILURE);
	}

	if (send_http_request(sock, host, path, host_len, path_len) != 0) {
		close(sock);
		exit(EXIT_FAILURE);
	}

	if (interactive) {
		set_non_canonical();
	}

	char buffer[HTTP_BUFFER_SIZE];
	ring_buffer_t rb;
	ring_buffer_init(&rb, buffer, sizeof(buffer));

	int connection_closed = 0;
	int first_page_shown = 0;
	fd_set read_fds;
	int max_fd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
	int running = 1;

	while (running) {
		FD_ZERO(&read_fds);

		if (!connection_closed) {
			FD_SET(sock, &read_fds);
		}
		if (interactive) {
			FD_SET(STDIN_FILENO, &read_fds);
		}

		if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
			perror("select");
			break;
		}

		if (!connection_closed && FD_ISSET(sock, &read_fds)) {
			int bytes = add_data_to_buffer(sock, &rb);
			if (bytes <= 0) {
				connection_closed = 1;
			}
		}

		int should_display = 0;
		if (!first_page_shown && interactive) {
			should_display = 1;
		} else if (!interactive) {
			should_display = 1;
		} else if (FD_ISSET(STDIN_FILENO, &read_fds)) {
			char ch;
			if (read(STDIN_FILENO, &ch, 1) > 0 && ch == ' ') {
				should_display = 1;
			}
		}

		if (should_display) {
			int result = display_page(&rb, interactive, connection_closed);
			if (result > 0 && !first_page_shown && interactive) {
				first_page_shown = 1;
			}
		}

		if (connection_closed && ring_buffer_used_space(&rb) == 0) {
			running = 0;
		}
	}

	if (interactive) {
		restore_canonical();
	}

	close(sock);
	exit(EXIT_SUCCESS);
}
