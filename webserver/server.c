/**
 * server.c -- A webserver written in C
 *
 * Test with curl (if you don't have it, install it):
 *
 *    curl -D - http://localhost:3490/
 *    curl -D - http://localhost:3490/d20
 *    curl -D - http://localhost:3490/date
 *
 * You can also test the above URLs in your browser! They should work!
 *
 * Posting Data:
 *
 *    curl -D - -X POST -H 'Content-Type: text/plain' -d 'Hello, sample data!'
 * http://localhost:3490/save
 *
 * (Posting data is harder to test from a browser.)
 */

//#include "cache.h"
//#include "file.h"
//#include "mime.h"
//#include "net.h"
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
#define MAX_RESPONSE_SIZE (1024*1024)
#define DEFAULT_MIME_TYPE "application/octet-stream"
#define BACKLOG 10 // how many pending connections queue will hold

struct file_data {
  int size;
  void *data;
};

/**
 * This gets an Internet address, either IPv4 or IPv6
 *
 * Helper function to make printing easier.
 */
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

/**
 * Return the main listening socket
 *
 * Returns -1 or error
 */
int get_listener_socket(char *port) {
  int sockfd;
  struct addrinfo hints, *servinfo, *p;
  int yes = 1;
  int rv;

  // This block of code looks at the local network interfaces and
  // tries to find some that match our requirements (namely either
  // IPv4 or IPv6 (AF_UNSPEC) and TCP (SOCK_STREAM) and use any IP on
  // this machine (AI_PASSIVE).

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return -1;
  }

  // Once we have a list of potential interfaces, loop through them
  // and try to set up a socket on each. Quit looping the first time
  // we have success.
  for (p = servinfo; p != NULL; p = p->ai_next) {

    // Try to make a socket based on this candidate interface
    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      // perror("server: socket");
      continue;
    }

    // SO_REUSEADDR prevents the "address already in use" errors
    // that commonly come up when testing servers.
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
      perror("setsockopt");
      close(sockfd);
      freeaddrinfo(servinfo); // all done with this structure
      return -2;
    }

    // See if we can bind this socket to this local IP address. This
    // associates the file descriptor (the socket descriptor) that
    // we will read and write on with a specific IP address.
    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      // perror("server: bind");
      continue;
    }

    // If we got here, we got a bound socket and we're done
    break;
  }

  freeaddrinfo(servinfo); // all done with this structure

  // If p is NULL, it means we didn't break out of the loop, above,
  // and we don't have a good socket.
  if (p == NULL) {
    fprintf(stderr, "webserver: failed to find local address\n");
    return -3;
  }

  // Start listening. This is what allows remote computers to connect
  // to this socket/IP.
  if (listen(sockfd, BACKLOG) == -1) {
    // perror("listen");
    close(sockfd);
    return -4;
  }

  return sockfd;
}

/* Lowercase a string */
char *strlower(char *s) {
  for (char *p = s; *p != '\0'; p++) {
    *p = tolower(*p);
  }
  return s;
}

/* Return a MIME type for a given filename */
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
  if (strcmp(ext, "png") == 0) {
    return "image/png";
  }

  return DEFAULT_MIME_TYPE;
}

/* Loads a file into memory and returns a pointer to the data
 *
 * Buffer is not NUL-terminated
 */
struct file_data *file_load(char *filename) {
  char *buffer, *p;
  struct stat buf;
  int bytes_read, bytes_remaining, total_bytes = 0;

  // Get the file size
  if (stat(filename, &buf) == -1) {
    return NULL;
  }

  // Make sure it's a regular file
  if (!(buf.st_mode & S_IFREG)) {
    return NULL;
  }

  // Open the file for reading
  FILE *fp = fopen(filename, "rb");

  if (fp == NULL) {
    return NULL;
  }

  // Allocate that many bytes
  bytes_remaining = buf.st_size;
  p = buffer = malloc(bytes_remaining);

  if (buffer == NULL) {
    return NULL;
  }

  // Read in the entire file
  while (bytes_read = fread(p, 1, bytes_remaining, fp),
         bytes_read != 0 && bytes_remaining > 0) {
    if (bytes_read == -1) {
      free(buffer);
      return NULL;
    }
    bytes_remaining -= bytes_read;
    p += bytes_read;
    total_bytes += bytes_read;
  }

  // Allocate the file data struct
  struct file_data *filedata = malloc(sizeof *filedata);

  if (filedata == NULL) {
    free(buffer);
    return NULL;
  }

  filedata->data = buffer;
  filedata->size = total_bytes;

  return filedata;
}

/* Free memory allocated by file_load() */
void file_free(struct file_data *filedata) {
  free(filedata->data);
  free(filedata);
}


/**
 * Send an HTTP response
 *
 * header:       "HTTP/1.1 404 NOT FOUND" or "HTTP/1.1 200 OK", etc.
 * content_type: "text/plain", etc.
 * body:         the data to send.
 *
 * Return the value from the send() function.
 */
int send_response(int fd, char *header, char *content_type, void *body,
                  int content_length) {
  //const int max_response_size = 65536;  // too small size for images!
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
  // Send it all!
  int rv = send(fd, response, response_length + content_length, 0);

  if (rv < 0) {
    perror("send");
  }

  return rv;
}

/**
 * Send a 404 response
 */
void resp_404(int fd) {
  char filepath[4096];
  struct file_data *filedata;
  char *mime_type;

  // Fetch the 404.html file
  snprintf(filepath, sizeof filepath, "%s/404.html", SERVER_FILES);
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

/**
 * Send a /d20 endpoint response
 */
void get_d20(int fd) {
  // !!!! IMPLEMENT ME
  //srand(time(NULL) + getpid());
  //char str[8];
  //int random = rand() % 20 + 1;
  //int length = sprintf(str, "%d\n", random);
  //send_response(fd, "HTTP/1.1 200 OK", "text/plain", str, length);
  
  char tmp_html[] = "<html><title>test html</title><body><h>에코 서버의 구현</h><br>Hello World!</body></html>";
 
  send_response(fd, "HTTP/1.1 200 OK", "text/html; charset=utf-8", tmp_html, strlen(tmp_html));


}

/**
 * Send a /date endpoint response
 */
// void get_date(int fd) {
//   // !!!! IMPLEMENT ME
//   time_t gmt_format;
//   time(&gmt_format);
//   char current[26]; // gmtime documentation stated that a user-supplied
//   buffer
//                     // should have at least 26 bytes.
//   int length = sprintf(current, "%s", asctime(gmtime(&gmt_format)));
//   send_response(fd, "HTTP/1.1 200 OK", "text/plain", current, length);
// }

/**
 * Post /save endpoint data
 */
void post_save(int fd, char *body) {
  char *status;

  // !!!! IMPLEMENT ME
  int file = open("data.txt", O_CREAT | O_WRONLY | O_APPEND, 0644);

  // lseek(file, SEEK_SET, SEEK_DATA);

  // int bytes_written = write(file, buffer, size);
  // if (bytes_written < 0)
  // {
  //   perror("write");
  // }
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
  // Save the body and send a response
}

// int get_file_or_cache(int fd, struct cache *cache, char *filepath) {
//   struct file_data *filedata;
//   struct cache_entry *cacheent;
//   char *mime_type;

//   cacheent = cache_get(cache, filepath);

//   if (cacheent != NULL) {
//     send_response(fd, "HTTP/1.1 200 OK", cacheent->content_type,
//                   cacheent->content, cacheent->content_length);
//   } else {
//     filedata = file_load(filepath);
//     if (filedata == NULL) {
//       return -1;
//     }

//     mime_type = mime_type_get(filepath);
//     send_response(fd, "HTTP/1.1 200 OK", mime_type, filedata->data,
//                   filedata->size);

//     cache_put(cache, filepath, mime_type, filedata->data, filedata->size);

//     file_free(filedata);
//   }

//   return 0;
// }

void get_file(int fd, char *request_path) {
  char filepath[65536];
  struct file_data *filedata;
  char *mime_type;

  //     // Try to find the file
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

/**
 * Search for the start of the HTTP body.
 *
 * The body is after the header, separated from it by a blank line (two newlines
 * in a row).
 *
 * "Newlines" in HTTP can be \r\n (carriage return followed by newline) or \n
 * (newline) or \r (carriage return).
 */
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

/**
 * Handle HTTP request and send response
 */
void handle_http_request(int fd) {
  const int request_buffer_size = 65536; // 64K
  char request[request_buffer_size];
  char *p;
  char request_type[8];       // GET or POST
  char request_path[1024];    // /info etc.
  char request_protocol[128]; // HTTP/1.1

  // Read request
  int bytes_recvd = recv(fd, request, request_buffer_size - 1, 0);

  if (bytes_recvd < 0) {
    perror("recv");
    return;
  }

  // NUL terminate request string
  request[bytes_recvd] = '\0';

  p = find_start_of_body(request);

  char *body = p + 1;
  // !!!! IMPLEMENT ME
  // Get the request type and path from the first line
  // Hint: sscanf()!
  sscanf(request, "%s %s %s", request_type, request_path, request_protocol);
  printf("Request: %s %s %s\n", request_type, request_path, request_protocol);
  
  // !!!! IMPLEMENT ME (stretch goal)

  // !!!! IMPLEMENT ME
  // call the appropriate handler functions, above, with the incoming data
  if (strcmp(request_type, "GET") == 0) {
    if (strcmp(request_path, "/d20") == 0) {
      get_d20(fd);
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
char *get_in_addr(const struct sockaddr *sa, char *s, size_t maxlen);
/**
 * Main
 */
int main(void) {
  int newfd; // listen on sock_fd, new connection on newfd
  struct sockaddr_storage their_addr; // connector's address information
  char s[INET6_ADDRSTRLEN];

  // Start reaping child processes
  // start_reaper();

  //struct cache *cache = cache_create(10, 0);

  // Get a listening socket
  int listenfd = get_listener_socket(PORT);

  if (listenfd < 0) {
    fprintf(stderr, "webserver: fatal error getting listening socket\n");
    exit(1);
  }

  printf("webserver: waiting for connections on port %s...\n", PORT);

  // This is the main loop that accepts incoming connections and
  // fork()s a handler process to take care of it. The main parent
  // process then goes back to waiting for new connections.

  while (1) {
    socklen_t sin_size = sizeof their_addr;

    // Parent process will block on the accept() call until someone
    // makes a new connection:
    newfd = accept(listenfd, (struct sockaddr *)&their_addr, &sin_size);
    if (newfd == -1) {
      perror("accept");
      continue;
    }

    // Print out a message that we got the connection
    get_in_addr(((struct sockaddr *)&their_addr), s, sizeof s);
    printf("server: got connection from %s\n", s);

    // newfd is a new socket descriptor for the new connection.
    // listenfd is still listening for new connections.

    handle_http_request(newfd);

    close(newfd);
  }

  // Unreachable code

  return 0;
}
