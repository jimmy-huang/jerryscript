/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "jerryscript-port.h"

#ifdef JERRY_DEBUGGER


typedef struct {
	int fd; /**< holds the file descriptor of the socket communication */
} jerry_socket_t;

static jerry_socket_t socket_connection; /**< client connection */

/**
 * Default implementation of jerry_port_accept_connection. This implementation
 * uses a socket API which is not yet supported by jerry-libc so the standard
 * libc is used instead.
 *
 * @return the socket that holds the incoming client connection
 *
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled with the JERRY_DEBUGGER macro.
 */
jerry_conn_t
jerry_port_accept_connection (int port) /**< connection port */
{
  int server_socket;
  struct sockaddr_in addr;
  socklen_t sin_size = sizeof (struct sockaddr_in);

  addr.sin_family = AF_INET;
  addr.sin_port = htons (port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if ((server_socket = socket (AF_INET, SOCK_STREAM, 0)) == -1)
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return NULL;
  }

  int opt_value = 1;

  if (setsockopt (server_socket, SOL_SOCKET, SO_REUSEADDR, &opt_value, sizeof (int)) == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return NULL;
  }

  if (bind (server_socket, (struct sockaddr *)&addr, sizeof (struct sockaddr)) == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return NULL;
  }

  if (listen (server_socket, 1) == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return NULL;
  }

  int client_socket = accept (server_socket, (struct sockaddr *)&addr, &sin_size);
  close (server_socket);

  if (client_socket == -1) {
    return NULL;
  }

  /* Set non-blocking mode. */
  int socket_flags = fcntl (client_socket, F_GETFL, 0);

  if (socket_flags < 0)
  {
    close (client_socket);
    return NULL;
  }

  if (fcntl (client_socket, F_SETFL, socket_flags | O_NONBLOCK) == -1)
  {
    close (client_socket);
    return NULL;
  }

  jerry_port_log (JERRY_LOG_LEVEL_DEBUG, "Connected from: %s\n", inet_ntoa (addr.sin_addr));

  socket_connection.fd = client_socket;

  return &socket_connection;
} /* jerry_port_socket_accept_connection */

/**
 * Default implementation of jerry_port_connection_send.
 * Send message to the client side.
 *
 * @return JERRY_CONN_ERROR_NONE - if the data was sent successfully to the client side
 *         JERRY_CONN_ERROR_INVALID - if the connection is invalid
 *         JERRY_CONN_ERROR_AGAIN - if the transfer didn't go through immediately, but can try again later
 *         JERRY_CONN_ERROR_IO - if the data failed to send
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled with the JERRY_DEBUGGER macro.
 */
jerry_conn_errors
jerry_port_connection_send (jerry_conn_t connection_p, /**< connection pointer */
                            const void *data_p, /**< data pointer */
                            size_t data_len, /**< data size */
                            ssize_t *bytes_sent) /**< bytes sent */
{
  if (!connection_p)
  {
    *bytes_sent = -1;
    return JERRY_CONN_ERROR_INVALID;
  }

  *bytes_sent = send (((jerry_socket_t *)connection_p)->fd, data_p, data_len, 0);

  if (*bytes_sent < 0)
  {
    if (errno == EWOULDBLOCK)
    {
      return JERRY_CONN_ERROR_AGAIN;
    }
    else
    {
      jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
      return JERRY_CONN_ERROR_IO;
    }
  }

  return JERRY_CONN_ERROR_NONE;
} /* jerry_port_connection_send */

/**
 * Default implementation of jerry_port_connection_receive.
 * Receive message from the client side.
 *
 * @return JERRY_CONN_ERROR_NONE - if the data was received successfully from the client side
 *         JERRY_CONN_ERROR_INVALID - if the connection is invalid
 *         JERRY_CONN_ERROR_AGAIN - if there's no incoming data, you should try again later
 *         JERRY_CONN_ERROR_IO - if the data failed to receive
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled with the JERRY_DEBUGGER macro.
 */
jerry_conn_errors
jerry_port_connection_receive(jerry_conn_t connection_p, /**< connection pointer */
                              void *data_p, /**< data pointer */
                              size_t max_len, /**< max data size */
                              ssize_t *bytes_received) /**< bytes received */
{
  if (!connection_p)
  {
    *bytes_received = -1;
    return JERRY_CONN_ERROR_INVALID;
  }

  *bytes_received = recv (((jerry_socket_t *)connection_p)->fd, data_p, max_len, 0);

  if (*bytes_received < 0)
  {
    if (errno == EWOULDBLOCK)
    {
      return JERRY_CONN_ERROR_AGAIN;
    }
    else
    {
      jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
      return JERRY_CONN_ERROR_IO;
    }
  }

  return JERRY_CONN_ERROR_NONE;
} /* jerry_port_connection_receive */

/**
 * Default implementation of jerry_port_close_connection.
 * Closes the debugger connection.
 *
 * @return JERRY_CONN_ERROR_NONE - if successful
 *         JERRY_CONN_ERROR_INVALID - if the connection is invalid
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled with the JERRY_DEBUGGER macro.
 */
jerry_conn_errors
jerry_port_close_connection (jerry_conn_t connection_p) /**< connection pointer */
{
  if (!connection_p)
  {
    return JERRY_CONN_ERROR_INVALID;
  }

  close (((jerry_socket_t *)connection_p)->fd);
  return JERRY_CONN_ERROR_NONE;
} /* jerry_port_close_connection */

#endif /* JERRY_DEBUGGER */
