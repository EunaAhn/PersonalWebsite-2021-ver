/**
 * httpserver.c
 * 
 * httpserver with c language and basic socket api.
 * 
**/

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#define PORT "8080" // the port users will be connecting to
#define SERVER_FILES "./messages"
#define SERVER_ROOT "./public_html"
#define MAX_RESPONSE_SIZE (1024*1024) // max response size, need to set big enough
#define DEFAULT_MIME_TYPE "application/octet-stream"
#define BACKLOG 10 // how many pending connections queue will hold

struct file_data {
  int size;
  void *data;
};

char *get_in_addr(const struct sockaddr *sa, char *s, size_t maxlen) {
  switch (sa->sa_family) {
  case AF_INET:
    inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen);
    break;
  case AF_INET6:
    inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen);
    break;
  default:
    strncpy(s, "Unknown AF", maxlen);
    return NULL;
  }
  return s;
}

int get_listener_socket(char *port) {
  int sockfd;
  struct addrinfo hints, *servinfo, *p;
  int yes = 1;
  int rv;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return -1;
  }

  for (p = servinfo; p != NULL; p = p->ai_next) {

    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror("setsockopt");
      close(sockfd);
      freeaddrinfo(servinfo); // all done with this structure
      return -2;
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      // perror("server: bind");
      continue;
    }

    break;
  }

  freeaddrinfo(servinfo); // all done with this structure

  if (p == NULL) {
    fprintf(stderr, "webserver: failed to find local address\n");
    return -3;
  }

  if (listen(sockfd, BACKLOG) == -1) {
    close(sockfd);
    return -4;
  }

  return sockfd;
}

char *strlower(char *s) {
  for (char *p = s; *p != '\0'; p++) {
    *p = tolower(*p);
  }
  return s;
}

char *mime_type_get(char *filename) {
  char *ext = strrchr(filename, '.');

  if (ext == NULL) {
    return DEFAULT_MIME_TYPE;
  }
  ext++;
  strlower(ext);
  // TODO: This is O(n) and should be O(1)

  if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) {
    return "text/html";
  }
  if (strcmp(ext, "jpeg") == 0 || strcmp(ext, "jpg") == 0) {
    return "image/jpg";
  }
  if (strcmp(ext, "css") == 0) {
    return "text/css";
  }
  if (strcmp(ext, "js") == 0) {
    return "application/javascript";
  }
  if (strcmp(ext, "json") == 0) {
    return "application/json";
  }
  if (strcmp(ext, "txt") == 0) {
    return "text/plan";
  }
  if (strcmp(ext, "gif") == 0) {
    return "image/gif";
  }
  if (strcmp(ext, "mp3") == 0) {
    return "audio/mpeg";
  }

  return DEFAULT_MIME_TYPE;
}

struct file_data *file_load(char *filename) {
  char *buffer, *p;
  struct stat buf;
  int bytes_read, bytes_remaining, total_bytes = 0;

  if (stat(filename, &buf) == -1) {   // Get the file size
    return NULL;
  }
  
  if (!(buf.st_mode & S_IFREG)) { // Make sure it's a regular file
    return NULL;
  }

  FILE *fp = fopen(filename, "rb"); // Open the file for reading

  if (fp == NULL) {
    return NULL;
  }

  bytes_remaining = buf.st_size;  // Allocate that many bytes
  p = buffer = malloc(bytes_remaining);

  if (buffer == NULL) {
    return NULL;
  }

  while (bytes_read = fread(p, 1, bytes_remaining, fp),
         bytes_read != 0 && bytes_remaining > 0) {   // Read in the entire file
    if (bytes_read == -1) {
      free(buffer);
      return NULL;
    }
    bytes_remaining -= bytes_read;
    p += bytes_read;
    total_bytes += bytes_read;
  }

  struct file_data *filedata = malloc(sizeof *filedata);   // Allocate the file data struct

  if (filedata == NULL) {
    free(buffer);
    return NULL;
  }

  filedata->data = buffer;
  filedata->size = total_bytes;

  return filedata;
}

void file_free(struct file_data *filedata) { /* Free memory allocated by file_load() */
  free(filedata->data);
  free(filedata);
}

int send_response(int fd, char *header, char *content_type, void *body,
                  int content_length) {
  const int max_response_size = MAX_RESPONSE_SIZE;
  char response[max_response_size];

  time_t rawtime;
  struct tm *info;

  info = localtime(&rawtime);

  int response_length =
      sprintf(response,
              "%s\n"
              "Date: %s"
              "Connection: close\n"
              "Content-Length: %d\n"
              "Content-Type: %s\n"
              "\n",
              header, asctime(info), content_length, content_type);
             
  memcpy(response + response_length, body, content_length);

  int rv = send(fd, response, response_length + content_length, 0);  // Send it all!

  if (rv < 0) {
    perror("send");
  }

  return rv;
}

void resp_404(int fd) {
  char filepath[4096];
  struct file_data *filedata;
  char *mime_type;

  snprintf(filepath, sizeof filepath, "%s/404.html", SERVER_FILES);  // Fetch the 404.html file
  filedata = file_load(filepath);

  if (filedata == NULL) {
    fprintf(stderr, "Cannot find system 404 file\n");
    exit(3);
  }

  mime_type = mime_type_get(filepath);

  send_response(fd, "HTTP/1.1 404 NOT FOUND", mime_type, filedata->data,
                filedata->size);

  file_free(filedata);
}

void get_addressbook(int fd) {
  // !!!! IMPLEMENT ME
  //srand(time(NULL) + getpid());
  //char str[8];
  //int random = rand() % 20 + 1;
  //int length = sprintf(str, "%d\n", random);
  //send_response(fd, "HTTP/1.1 200 OK", "text/plain", str, length);
  char tmp_html[] = "<html><title>test html</title><body>Hello World!</body></html>";
  send_response(fd, "HTTP/1.1 200 OK", "text/html; charset=utf-8", tmp_html, strlen(tmp_html));
}

void post_save(int fd, char *body) {
  char *status;
  int file = open("data.txt", O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (file < 0) {
    status = "failed";
  } else {
    flock(file, LOCK_EX);
    write(file, body, strlen(body));
    flock(file, LOCK_UN);
    close(file);
    status = "ok";
  }

  char response_body[128];
  int length = sprintf(response_body, "{\"status\": \"%s\"}\n", status);

  send_response(fd, "HTTP/1.1 200 OK", "application/json", response_body,
                length);
}

void get_file(int fd, char *request_path) {
  char filepath[65536];
  struct file_data *filedata;
  char *mime_type;

  snprintf(filepath, sizeof(filepath), "%s%s", SERVER_ROOT, request_path);
  filedata = file_load(filepath);

  if (filedata == NULL) {
    snprintf(filepath, sizeof filepath, "%s%s/index.html", SERVER_ROOT,
             request_path);
    filedata = file_load(filepath);

    if (filedata == NULL) {
      resp_404(fd);
      return;
    }
  }

  mime_type = mime_type_get(filepath);
  printf("mimeType: %s\n", mime_type);
  send_response(fd, "HTTP/1.1 200 OK", mime_type, filedata->data,
                filedata->size);

  file_free(filedata);
}

char *find_start_of_body(char *header) {
  char *start;

  if ((start = strstr(header, "\r\n\r\n")) != NULL) {
    return start + 2;
  } else if ((start = strstr(header, "\n\n")) != NULL) {
    return start + 2;
  } else if ((start = strstr(header, "\r\r")) != NULL) {
    return start + 2;
  } else {
    return start;
  }
}

void handle_http_request(int fd) {
  const int request_buffer_size = 65536; // 64K
  char request[request_buffer_size];
  char *p;
  char request_type[8];       // GET or POST
  char request_path[1024];    // /info etc.
  char request_protocol[128]; // HTTP/1.1

  int bytes_recvd = recv(fd, request, request_buffer_size - 1, 0);   // Read request

  if (bytes_recvd < 0) {
    perror("recv");
    return;
  }
  
  request[bytes_recvd] = '\0'; // NUL terminate request string
  
  printf("REQ: %s\n", request);

  p = find_start_of_body(request);

  char *body = p + 1;

  sscanf(request, "%s %s %s", request_type, request_path, request_protocol);
  printf("Request: %s %s %s\n", request_type, request_path, request_protocol);
  
  if (strcmp(request_type, "GET") == 0) {
    if (strcmp(request_path, "/d20") == 0) {
      get_addressbook(fd);
    } else {
      printf("getFilename: %s\n", request_path);
      get_file(fd, request_path);
    }
  } else if (strcmp(request_type, "POST") == 0) {
    if (strcmp(request_path, "/save") == 0) {
      post_save(fd, body);
    } else {
      resp_404(fd);
    }
  } else {
    fprintf(stderr, "Unknown request type \"%s\"\n", request_type);
    return;
  }
}

int main(void) {
  int newfd; // listen on sock_fd, new connection on newfd
  struct sockaddr_storage their_addr; // connector's address information
  char s[INET6_ADDRSTRLEN];

  int listenfd = get_listener_socket(PORT);

  if (listenfd < 0) {
    fprintf(stderr, "webserver: fatal error getting listening socket\n");
    exit(1);
  }

  printf("webserver: waiting for connections on port %s...\n", PORT);

  while (1) {
    socklen_t sin_size = sizeof their_addr;

    newfd = accept(listenfd, (struct sockaddr *)&their_addr, &sin_size);
    if (newfd == -1) {
      perror("accept");
      continue;
    }

    get_in_addr(((struct sockaddr *)&their_addr), s, sizeof s);
    printf("server: got connection from %s\n", s);

    handle_http_request(newfd);

    close(newfd);
  }
  // Unreachable code
  return 0;
}
