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

#include "jerryscript-port.h"
#include "jmem.h"

#ifdef JERRY_DEBUGGER

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* JerryScript debugger protocol is a simplified version of RFC-6455 (WebSockets). */

/**
 * Last fragment of a Websocket package.
 */
#define JERRY_DEBUGGER_WEBSOCKET_FIN_BIT 0x80

/**
 * Masking-key is available.
 */
#define JERRY_DEBUGGER_WEBSOCKET_MASK_BIT 0x80

/**
 * Opcode type mask.
 */
#define JERRY_DEBUGGER_WEBSOCKET_OPCODE_MASK 0x0fu

/**
 * Packet length mask.
 */
#define JERRY_DEBUGGER_WEBSOCKET_LENGTH_MASK 0x7fu

/**
 * Maximum number of bytes transmitted or received.
 */
#define JERRY_DEBUGGER_MAX_BUFFER_SIZE 128

/**
 * Size of websocket header size.
 */
#define JERRY_DEBUGGER_WEBSOCKET_HEADER_SIZE 2

/**
 * Payload mask size in bytes of a websocket package.
 */
#define JERRY_DEBUGGER_WEBSOCKET_MASK_SIZE 4

/**
 * Maximum message size with 1 byte size field.
 */
#define JERRY_DEBUGGER_WEBSOCKET_ONE_BYTE_LEN_MAX 125

/**
 * Waiting for data from the client.
 */
#define JERRY_DEBUGGER_RECEIVE_DATA_MODE \
  (JERRY_DEBUGGER_BREAKPOINT_MODE | JERRY_DEBUGGER_CLIENT_SOURCE_MODE)

/**
 * WebSocket opcode types.
 */
typedef enum
{
  JERRY_DEBUGGER_WEBSOCKET_TEXT_FRAME = 1, /**< text frame */
  JERRY_DEBUGGER_WEBSOCKET_BINARY_FRAME = 2, /**< binary frame */
  JERRY_DEBUGGER_WEBSOCKET_CLOSE_CONNECTION = 8, /**< close connection */
  JERRY_DEBUGGER_WEBSOCKET_PING = 9, /**< ping (keep alive) frame */
  JERRY_DEBUGGER_WEBSOCKET_PONG = 10, /**< reply to ping frame */
} jerry_websocket_opcode_type_t;

/**
 * Header for incoming packets.
 */
typedef struct
{
  uint8_t ws_opcode; /**< websocket opcode */
  uint8_t size; /**< size of the message */
  uint8_t mask[4]; /**< mask bytes */
} jerry_debugger_receive_header_t;

static uint16_t debugger_port; /**< debugger socket communication port */
static int fd; /**< holds the file descriptor of the socket communication */

void jerry_debugger_compute_sha1 (const uint8_t *input1, size_t input1_len,
                                  const uint8_t *input2, size_t input2_len,
                                  uint8_t output[20]);

/**
 * Close the socket connection to the client.
 */
static void
jerry_debugger_close_connection_tcp (bool log_error) /**< log error */
{
  if (log_error)
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
  }

  if (fd != -1)
  {
    close (fd);
    fd = -1;
  }

  jerry_port_log (JERRY_LOG_LEVEL_DEBUG, "Debugger client connection closed.\n");
} /* jerry_debugger_close_connection_tcp */

/**
 * Send message to the client side.
 *
 * @return true - if the data was sent successfully to the client side
 *         false - otherwise
 */
static bool
jerry_debugger_send_tcp (const uint8_t *data_p, /**< data pointer */
                         size_t data_size) /**< data size */
{
  do
  {
    ssize_t sent_bytes = send (fd, data_p, data_size, 0);

    if (sent_bytes < 0)
    {
      if (errno == EWOULDBLOCK)
      {
        continue;
      }

      return false;
    }

    data_size -= (size_t) sent_bytes;
    data_p += sent_bytes;
  }
  while (data_size > 0);

  return true;
} /* jerry_debugger_send_tcp */

/**
 * Convert a 6-bit value to a Base64 character.
 *
 * @return Base64 character
 */
static uint8_t
jerry_to_base64_character (uint8_t value) /**< 6-bit value */
{
  if (value < 26)
  {
    return (uint8_t) (value + 'A');
  }

  if (value < 52)
  {
    return (uint8_t) (value - 26 + 'a');
  }

  if (value < 62)
  {
    return (uint8_t) (value - 52 + '0');
  }

  if (value == 62)
  {
    return (uint8_t) '+';
  }

  return (uint8_t) '/';
} /* jerry_to_base64_character */

/**
 * Encode a byte sequence into Base64 string.
 */
static void
jerry_to_base64 (const uint8_t *source_p, /**< source data */
                 uint8_t *destination_p, /**< destination buffer */
                 size_t length) /**< length of source, must be divisible by 3 */
{
  while (length >= 3)
  {
    uint8_t value = (source_p[0] >> 2);
    destination_p[0] = jerry_to_base64_character (value);

    value = (uint8_t) (((source_p[0] << 4) | (source_p[1] >> 4)) & 0x3f);
    destination_p[1] = jerry_to_base64_character (value);

    value = (uint8_t) (((source_p[1] << 2) | (source_p[2] >> 6)) & 0x3f);
    destination_p[2] = jerry_to_base64_character (value);

    value = (uint8_t) (source_p[2] & 0x3f);
    destination_p[3] = jerry_to_base64_character (value);

    source_p += 3;
    destination_p += 4;
    length -= 3;
  }
} /* jerry_to_base64 */

/**
 * Process WebSocket handshake.
 *
 * @return true - if the handshake was completed successfully
 *         false - otherwise
 */
static bool
jerry_process_handshake (int client_socket, /**< client socket */
                         uint8_t *request_buffer_p) /**< temporary buffer */
{
  size_t request_buffer_size = 1024;
  uint8_t *request_end_p = request_buffer_p;

  /* Buffer request text until the double newlines are received. */
  while (true)
  {
    size_t length = request_buffer_size - 1u - (size_t) (request_end_p - request_buffer_p);

    if (length == 0)
    {
      jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Handshake buffer too small.\n");
      return false;
    }

    ssize_t size = recv (client_socket, request_end_p, length, 0);

    if (size < 0)
    {
      jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
      return false;
    }

    request_end_p += (size_t) size;
    *request_end_p = 0;

    if (request_end_p > request_buffer_p + 4
        && memcmp (request_end_p - 4, "\r\n\r\n", 4) == 0)
    {
      break;
    }
  }

  /* Check protocol. */
  const char *text_p = "GET /jerry-debugger";
  size_t text_len = strlen (text_p);

  if ((size_t) (request_end_p - request_buffer_p) < text_len
      || memcmp (request_buffer_p, text_p, text_len) != 0)
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Invalid handshake format.\n");
    return false;
  }

  uint8_t *websocket_key_p = request_buffer_p + text_len;

  text_p = "Sec-WebSocket-Key:";
  text_len = strlen (text_p);

  while (true)
  {
    if ((size_t) (request_end_p - websocket_key_p) < text_len)
    {
      jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Sec-WebSocket-Key not found.\n");
      return false;
    }

    if (websocket_key_p[0] == 'S'
        && websocket_key_p[-1] == '\n'
        && websocket_key_p[-2] == '\r'
        && memcmp (websocket_key_p, text_p, text_len) == 0)
    {
      websocket_key_p += text_len;
      break;
    }

    websocket_key_p++;
  }

  /* String terminated by double newlines. */

  while (*websocket_key_p == ' ')
  {
    websocket_key_p++;
  }

  uint8_t *websocket_key_end_p = websocket_key_p;

  while (*websocket_key_end_p > ' ')
  {
    websocket_key_end_p++;
  }

  /* Since the request_buffer_p is not needed anymore it can
   * be reused for storing the SHA-1 key and Base64 string. */

  const size_t sha1_length = 20;

  jerry_debugger_compute_sha1 (websocket_key_p,
                               (size_t) (websocket_key_end_p - websocket_key_p),
                               (const uint8_t *) "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                               36,
                               request_buffer_p);

  /* The SHA-1 key is 20 bytes long but jerry_to_base64 expects
   * a length divisible by 3 so an extra 0 is appended at the end. */
  request_buffer_p[sha1_length] = 0;

  jerry_to_base64 (request_buffer_p, request_buffer_p + sha1_length + 1, sha1_length + 1);

  /* Last value must be replaced by equal sign. */

  text_p = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";

  if (!jerry_debugger_send_tcp ((const uint8_t *) text_p, strlen (text_p))
      || !jerry_debugger_send_tcp (request_buffer_p + sha1_length + 1, 27))
  {
    return false;
  }

  text_p = "=\r\n\r\n";
  return jerry_debugger_send_tcp ((const uint8_t *) text_p, strlen (text_p));
} /* jerry_process_handshake */

/**
 * Default implementation of debugger accept_connection api. This implementation
 * uses a socket API which is not yet supported by jerry-libc so the standard
 * libc is used instead.
 *
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled with the JERRY_DEBUGGER macro.
 *
 * @return true - if the connection succeeded
 *         false - otherwise
 */
static bool
jerry_debugger_accept_connection_ws (struct jerry_debugger_transport_t *transport_p) /**< transport object */
{
  JERRY_UNUSED (transport_p);

  int server_socket;
  struct sockaddr_in addr;
  socklen_t sin_size = sizeof (struct sockaddr_in);

  addr.sin_family = AF_INET;
  addr.sin_port = htons (debugger_port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if ((server_socket = socket (AF_INET, SOCK_STREAM, 0)) == -1)
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return false;
  }

  int opt_value = 1;

  if (setsockopt (server_socket, SOL_SOCKET, SO_REUSEADDR, &opt_value, sizeof (int)) == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return false;
  }

  if (bind (server_socket, (struct sockaddr *)&addr, sizeof (struct sockaddr)) == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return false;
  }

  if (listen (server_socket, 1) == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return false;
  }

  jerry_port_log (JERRY_LOG_LEVEL_DEBUG, "Waiting for client connection\n");

  fd = accept (server_socket, (struct sockaddr *)&addr, &sin_size);

  if (fd == -1)
  {
    close (server_socket);
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: %s\n", strerror (errno));
    return false;
  }

  close (server_socket);

  bool is_handshake_ok = false;

  JMEM_DEFINE_LOCAL_ARRAY (request_buffer_p, 1024, uint8_t);

  is_handshake_ok = jerry_process_handshake (fd, request_buffer_p);

  JMEM_FINALIZE_LOCAL_ARRAY (request_buffer_p);

  if (!is_handshake_ok)
  {
    jerry_debugger_close_connection_tcp (false);
    return false;
  }

  /* Set non-blocking mode. */
  int socket_flags = fcntl (fd, F_GETFL, 0);

  if (socket_flags < 0)
  {
    jerry_debugger_close_connection_tcp (true);
    return false;
  }

  if (fcntl (fd, F_SETFL, socket_flags | O_NONBLOCK) == -1)
  {
    jerry_debugger_close_connection_tcp (true);
    return false;
  }

  jerry_port_log (JERRY_LOG_LEVEL_DEBUG, "Connected from: %s\n", inet_ntoa (addr.sin_addr));

  return true;
} /* jerry_debugger_accept_connection_ws */

/**
 * Default implementation of debugger close_connection api.
 * Close the socket connection to the client.
 */
static inline void __attr_always_inline___
jerry_debugger_close_connection_ws (struct jerry_debugger_transport_t *transport_p) /**< transport object */
{
  JERRY_UNUSED (transport_p);

  jerry_debugger_close_connection_tcp (false);
} /* jerry_debugger_close_connection_ws */

/**
 * Default implementation of debugger send api.
 * Send message to the client side.
 *
 * Note:
 *   This function is only available if the port implementation library is
 *    compiled with the JERRY_DEBUGGER macro.
 *
 * @return true - if the data was sent successfully to the client side
 *         false - otherwise
 */
static bool
jerry_debugger_send_ws (struct jerry_debugger_transport_t *transport_p, /**< transport object */
                        uint8_t *message_data_p, /**< send data pointer */
                        size_t data_size) /**< send data size */
{
  JERRY_UNUSED (transport_p);

  uint8_t *header_p = message_data_p;
  header_p[0] = JERRY_DEBUGGER_WEBSOCKET_FIN_BIT | JERRY_DEBUGGER_WEBSOCKET_BINARY_FRAME;
  header_p[1] = (uint8_t) data_size;

  return jerry_debugger_send_tcp (header_p, data_size + JERRY_DEBUGGER_WEBSOCKET_HEADER_SIZE);
} /* jerry_debugger_send_ws */

/**
 * Default implementation of debugger receive api.
 * Receive message from the client side.
 *
 *   This function is only available if the port implementation library is
 *   compiled with the JERRY_DEBUGGER macro.
 *
 * @return true - if the data was received successfully from the client side,
 *         false - otherwise
 */
static bool
jerry_debugger_receive_ws (struct jerry_debugger_transport_t *transport_p, /**< transport object */
                           uint8_t *message_data_p, /**< received data pointer */
                           size_t *data_size, /**< [out] received data size */
                           uint32_t *data_offset) /**< [in/out] data buffer offset */
{
  JERRY_UNUSED (transport_p);

  uint8_t *recv_buffer_p = message_data_p;
  uint32_t offset = *data_offset;

  ssize_t byte_recv = recv (fd,
                            message_data_p + offset,
                            JERRY_DEBUGGER_MAX_BUFFER_SIZE - offset,
                            0);

  if (byte_recv < 0)
  {
    if (errno != EWOULDBLOCK)
    {
      return false;
    }

    byte_recv = 0;
  }

  offset += (uint32_t) byte_recv;
  *data_offset = offset;

  if (offset < sizeof (jerry_debugger_receive_header_t))
  {
    return true;
  }

  uint8_t receive_header_size = JERRY_DEBUGGER_WEBSOCKET_HEADER_SIZE + JERRY_DEBUGGER_WEBSOCKET_MASK_SIZE;
  uint8_t max_receive_size = (uint8_t) (JERRY_DEBUGGER_MAX_BUFFER_SIZE - receive_header_size);
  if ((recv_buffer_p[0] & ~JERRY_DEBUGGER_WEBSOCKET_OPCODE_MASK) != JERRY_DEBUGGER_WEBSOCKET_FIN_BIT
      || (recv_buffer_p[1] & JERRY_DEBUGGER_WEBSOCKET_LENGTH_MASK) > max_receive_size
      || !(recv_buffer_p[1] & JERRY_DEBUGGER_WEBSOCKET_MASK_BIT))
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Unsupported Websocket message.\n");
    return false;
  }

  if ((recv_buffer_p[0] & JERRY_DEBUGGER_WEBSOCKET_OPCODE_MASK) != JERRY_DEBUGGER_WEBSOCKET_BINARY_FRAME)
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Unsupported Websocket opcode.\n");
    return false;
  }

  uint32_t message_size = (uint32_t) (recv_buffer_p[1] & JERRY_DEBUGGER_WEBSOCKET_LENGTH_MASK);
  uint32_t message_total_size = (uint32_t) (message_size + sizeof (jerry_debugger_receive_header_t));

  if (offset < message_total_size)
  {
    return true;
  }

  /* Unmask data bytes. */
  uint8_t *data_p = recv_buffer_p + sizeof (jerry_debugger_receive_header_t);
  const uint8_t *mask_p = data_p - JERRY_DEBUGGER_WEBSOCKET_MASK_SIZE;
  const uint8_t *mask_end_p = data_p;
  const uint8_t *data_end_p = data_p + message_size;

  while (data_p < data_end_p)
  {
    /* Invert certain bits with xor operation. */
    *data_p = *data_p ^ *mask_p;

    data_p++;
    mask_p++;

    if (mask_p >= mask_end_p)
    {
      mask_p -= JERRY_DEBUGGER_WEBSOCKET_MASK_SIZE;
    }
  }

  *data_size = (size_t) message_size;

  return true;
} /* jerry_debugger_receive_ws */

static struct jerry_debugger_transport_t socket_transport =
{
  .send_header_size = JERRY_DEBUGGER_WEBSOCKET_HEADER_SIZE,
  .receive_header_size = JERRY_DEBUGGER_WEBSOCKET_HEADER_SIZE + JERRY_DEBUGGER_WEBSOCKET_MASK_SIZE,
  .max_message_size = JERRY_DEBUGGER_WEBSOCKET_ONE_BYTE_LEN_MAX,
  .accept_connection = jerry_debugger_accept_connection_ws,
  .close_connection = jerry_debugger_close_connection_ws,
  .send = jerry_debugger_send_ws,
  .receive = jerry_debugger_receive_ws,
};

#endif /* JERRY_DEBUGGER */

/**
 * Create and return the socket transport on the provided port for the debugger
 *
 * @return the transport created
 */
struct jerry_debugger_transport_t *
jerry_port_init_socket_transport (uint16_t tcp_port) /**< server port number */
{
#ifdef JERRY_DEBUGGER
  debugger_port = tcp_port;
  return &socket_transport;
#else /* !JERRY_DEBUGGER */
  JERRY_UNUSED (tcp_port);
  return NULL;
#endif /* JERRY_DEBUGGER */
} /* jerry_port_init_socket_transport */
