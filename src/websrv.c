/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <microhttpd.h>

#include "asset.h"
#include "fs.h"
#include "sys.h"
#include "websrv.h"


enum MHD_Result
websrv_queue_response(struct MHD_Connection *conn, unsigned int status,
		      struct MHD_Response *resp) {
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
  			  "*");

  return MHD_queue_response(conn, status, resp);
}


/**
 * Respond to a launch request.
 **/
static enum MHD_Result
launch_request(struct MHD_Connection *conn) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char* title_id;
  unsigned int status;
  const char *args;

  title_id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "titleId");
  args = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "args");

  if(!title_id) {
    status = MHD_HTTP_BAD_REQUEST;
  } else if(sys_launch_title(title_id, args)) {
    status = MHD_HTTP_SERVICE_UNAVAILABLE;
  } else {
    status = MHD_HTTP_OK;
  }

  if((resp=MHD_create_response_from_buffer(0, "",
					   MHD_RESPMEM_PERSISTENT))) {
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


/**
 * Respond to a homebrew loading request.
 **/
static enum MHD_Result
hbldr_request(struct MHD_Connection *conn) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char* path;
  const char *args;
  const char *pipe;
  int fd;

  path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
  args = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "args");
  pipe = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "pipe");

  if(!path) {
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_BAD_REQUEST, resp);
      MHD_destroy_response(resp);
    }

  } else if((fd=sys_launch_homebrew(path, args)) < 0) {
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, resp);
      MHD_destroy_response(resp);
    }

  } else if(pipe && strcmp(pipe, "0")) {
    if((resp=MHD_create_response_from_pipe(fd))) {
      ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
    }
  } else {
    close(fd);
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
    }
  }

  return ret;
}


/**
 *
 **/
static enum MHD_Result
ahc_echo(void *cls, struct MHD_Connection *conn,
	 const char *url, const char *method,
	 const char *version, const char *upload_data,
	 size_t *upload_data_size, void **ptr) {
  static int aptr;

  if(strcmp(method, MHD_HTTP_METHOD_GET)) {
    return MHD_NO;
  }

  // never respond on first call
  if(&aptr != *ptr) {
    *ptr = &aptr;
    return MHD_YES;
  }

  // reset when done
  *ptr = NULL;

  if(!strcmp("/fs", url)) {
    return fs_request(conn, url);
  }
  if(!strncmp("/fs/", url, 4)) {
    return fs_request(conn, url);
  }

  if(!strcmp("/launch", url)) {
    return launch_request(conn);
  }

  if(!strcmp("/hbldr", url)) {
    return hbldr_request(conn);
  }

  if(!strcmp("/", url) || !url[0]) {
    return asset_request(conn, "/index.html");
  }

  return asset_request(conn, url);
}


int
websrv_listen(unsigned short port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  struct MHD_Daemon *httpd;
  char ip[INET_ADDRSTRLEN];
  struct ifaddrs *ifaddr;
  int ifaddr_wait = 1;
  socklen_t addr_len;
  int connfd;
  int srvfd;

  if(getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    exit(EXIT_FAILURE);
  }

  signal(SIGPIPE, SIG_IGN);

  // Enumerate all AF_INET IPs
  for(struct ifaddrs *ifa=ifaddr; ifa!=NULL; ifa=ifa->ifa_next) {
    if(ifa->ifa_addr == NULL) {
      continue;
    }

    if(ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    // skip localhost
    if(!strncmp("lo", ifa->ifa_name, 2)) {
      continue;
    }

    struct sockaddr_in *in = (struct sockaddr_in*)ifa->ifa_addr;
    inet_ntop(AF_INET, &(in->sin_addr), ip, sizeof(ip));

    // skip interfaces without an ip
    if(!strncmp("0.", ip, 2)) {
      continue;
    }

    printf("Serving on http://%s:%d (%s)\n", ip, port, ifa->ifa_name);
    ifaddr_wait = 0;
  }

  freeifaddrs(ifaddr);

  if(ifaddr_wait) {
    return 0;
  }

  if((srvfd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(srvfd);
    return -1;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    perror("bind");
    close(srvfd);
    return -1;
  }

  if(listen(srvfd, 5) != 0) {
    perror("listen");
    close(srvfd);
    return -1;
  }

  if(!(httpd=MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_ITC |
			      MHD_USE_NO_LISTEN_SOCKET | MHD_USE_DEBUG |
			      MHD_USE_INTERNAL_POLLING_THREAD,
			      0, NULL, NULL,
			      &ahc_echo, NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    close(srvfd);
    return -1;
  }

  while(1) {
    addr_len = sizeof(client_addr);
    if((connfd=accept(srvfd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
      perror("accept");
      break;
    }

    if(MHD_add_connection(httpd, connfd, (struct sockaddr*)&client_addr,
			  addr_len) != MHD_YES) {
      perror("MHD_add_connection");
      break;
    }
  }

  MHD_stop_daemon(httpd);

  return close(srvfd);
}
