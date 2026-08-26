#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop_server(int signal_number) {
	(void)signal_number;
	running = 0;
}

static int write_all(int fd, const void *data, size_t length) {
	const unsigned char *cursor = data;
	while (length) {
		ssize_t written = write(fd, cursor, length);
		if (written < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		cursor += (size_t)written;
		length -= (size_t)written;
	}
	return 0;
}

static const char *content_type(const char *name) {
	const char *extension = strrchr(name, '.');
	/* LMS uses audio/x-flac for its native FLAC path.  audio/flac is valid but
	 * can select a server-side transcoder and destroy DoP marker bytes. */
	if (extension && !strcmp(extension, ".flac")) return "audio/x-flac";
	if (extension && !strcmp(extension, ".dsf")) return "audio/x-dsf";
	return "application/octet-stream";
}

static void serve_client(int client) {
	char request[4096];
	char method[16];
	char target[1024];
	char version[16];
	char header[512];
	char buffer[65536];
	ssize_t received;
	int file_fd;
	struct stat info;
	const char *name;

	received = read(client, request, sizeof(request) - 1);
	if (received <= 0) return;
	request[received] = '\0';
	if (sscanf(request, "%15s %1023s %15s", method, target, version) != 3 ||
		(strcmp(method, "GET") && strcmp(method, "HEAD"))) {
		write_all(client, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 47);
		return;
	}
	(void)version;
	name = target[0] == '/' ? target + 1 : target;
	if (!*name || strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
		write_all(client, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n", 45);
		return;
	}
	file_fd = open(name, O_RDONLY);
	if (file_fd < 0 || fstat(file_fd, &info) || !S_ISREG(info.st_mode)) {
		if (file_fd >= 0) close(file_fd);
		write_all(client, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n", 45);
		return;
	}
	snprintf(header, sizeof(header),
		"HTTP/1.1 200 OK\r\nContent-Length: %lld\r\nContent-Type: %s\r\n"
		"Accept-Ranges: none\r\nConnection: close\r\n\r\n",
		(long long)info.st_size, content_type(name));
	if (write_all(client, header, strlen(header)) || !strcmp(method, "HEAD")) {
		close(file_fd);
		return;
	}
	for (;;) {
		ssize_t count = read(file_fd, buffer, sizeof(buffer));
		if (!count) break;
		if (count < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (write_all(client, buffer, (size_t)count)) break;
	}
	close(file_fd);
}

int main(int argc, char **argv) {
	int server;
	int enabled = 1;
	long port;
	char *end;
	struct sockaddr_in address;

	if (argc != 2) {
		fprintf(stderr, "usage: %s PORT\n", argv[0]);
		return 2;
	}
	errno = 0;
	port = strtol(argv[1], &end, 10);
	if (errno || *end || port < 1024 || port > 65535) return 2;
	signal(SIGINT, stop_server);
	signal(SIGTERM, stop_server);
	signal(SIGPIPE, SIG_IGN);
	/* Each LMS decoder request gets an independent process.  A transcoder may
	 * intentionally keep its source connection open for the track duration;
	 * it must never block rate transitions or the endurance fixture. */
	signal(SIGCHLD, SIG_IGN);
	server = socket(AF_INET, SOCK_STREAM, 0);
	if (server < 0) return 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 8)) {
		perror("fixture server");
		close(server);
		return 1;
	}
	while (running) {
		int client = accept(server, NULL, NULL);
		pid_t child;
		if (client < 0) {
			if (errno == EINTR) continue;
			break;
		}
		child = fork();
		if (child == 0) {
			close(server);
			serve_client(client);
			close(client);
			_exit(0);
		}
		close(client);
		if (child < 0) perror("fixture server fork");
	}
	close(server);
	return 0;
}
