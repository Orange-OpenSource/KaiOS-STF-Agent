/*
 * Copyright 2019 Orange
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wire.pb.h"
#include <fcntl.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <sys/socket.h>
#include <sys/un.h>

using namespace google::protobuf::io;

typedef struct {
  int socket;
  char *endpoint;
} connection_handler;

/*
 * Simple logging helper
 */
typedef enum _log_level { VERBOSE, DEBUG, INFO, WARNING, ERROR } log_level;
log_level current_log_level;
char* TAG;

static const char level_label[] = {'V', 'D', 'I', 'W', 'E'};

void log(log_level level, const char *tag, const char *fmt, ...) {
  if (level < current_log_level) {
    return;
  }
  time_t t = time(NULL);
  struct tm *current_time = localtime(&t);
  char time_string[16];
  strftime(time_string, sizeof(time_string), "%H:%M:%S", current_time);
  fprintf(stderr, "%s %d-%d %c/%s ", time_string, getpid(), getppid(),
          level_label[level], tag);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
}

/*
 * Create a localabstract socket
 * As stated in "man 7 unix" :
 * "an  abstract  socket  address  is distinguished (from a pathname
 * socket) by the fact that sun_path[0] is a null byte ('\0')"
 */
int create_socket(const char *name) {
  struct sockaddr_un addr;
  int s;
  s = socket(AF_LOCAL, SOCK_STREAM, 0);
  if (s == -1) {
    perror("initializing socket");
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_LOCAL;
  addr.sun_path[0] = 0;
  memcpy(addr.sun_path + 1, name, strlen(name));
  int addrlen = strlen(name) + offsetof(struct sockaddr_un, sun_path) + 1;
  if (bind(s, (sockaddr *)&addr, addrlen) == -1) {
    perror("binding socket");
    return -1;
  }
  return s;
}

/*
 * Receaves/Sends protobuf messages. Refer to those links for more informations:
 * https://stackoverflow.com/questions/2340730/are-there-c-equivalents-for-the-protocol-buffers-delimited-i-o-functions-in-ja
 * https://stackoverflow.com/questions/23280457/c-google-protocol-buffers-open-http-socket
 */
bool recv_message(google::protobuf::io::ZeroCopyInputStream *rawInput,
                google::protobuf::MessageLite *message) {
  google::protobuf::io::CodedInputStream input(rawInput);
  uint32_t size;
  if (!input.ReadVarint32(&size))
    return false;

  google::protobuf::io::CodedInputStream::Limit limit = input.PushLimit(size);

  if (!message->MergeFromCodedStream(&input))
    return false;
  if (!input.ConsumedEntireMessage())
    return false;

  input.PopLimit(limit);
  return true;
}

void send_message(stf::Envelope &envelope,
                   google::protobuf::io::ZeroCopyOutputStream *coded_output) {
  google::protobuf::io::CodedOutputStream output(coded_output);
  const int size = envelope.ByteSize();
  output.WriteVarint32(size);
  envelope.SerializeWithCachedSizes(&output);
}

/*
 * Returns a response message according to the request message
 */
stf::Envelope process_message(stf::Envelope &request) {
  stf::Envelope response;
  if (request.type() == stf::MessageType::GET_DISPLAY) {

    response.set_id(request.id());
    response.set_type(stf::MessageType::GET_DISPLAY);

    std::string data;
    stf::GetDisplayResponse display_response;
    display_response.set_success(true);
    display_response.set_width(240);
    display_response.set_height(320);
    display_response.set_secure(false);
    display_response.set_xdpi(240);
    display_response.set_ydpi(320);
    display_response.set_fps(2);
    display_response.set_rotation(0);
    display_response.set_density(280);
    display_response.SerializeToString(&data);

    response.set_message(data);
  } // TODO: add other request types
  return response;
}

int receive_data(int socket) {
  FileInputStream *is = new FileInputStream(socket);
  FileOutputStream *os = new FileOutputStream(socket);
  char buffer[512];
  int bytecount = recv(socket, buffer, 512, MSG_PEEK);

  log(DEBUG, TAG, "read %d bytes", bytecount);
  if (bytecount > 0) {
    stf::Envelope input_msg;
    if (recv_message(is, &input_msg)) {
      log(DEBUG, TAG, "received:\n %s", input_msg.DebugString().c_str());
      stf::Envelope output_msg = process_message(input_msg);
      if (output_msg.ByteSize() > 0) {
        log(DEBUG, TAG, "sending (%d):\n %s", output_msg.ByteSize(),
            output_msg.DebugString().c_str());
        send_message(output_msg, os);
        os->Flush();
      } else {
        log(WARNING, TAG, "could not process message");
      }
    } else {
      log(ERROR, TAG, "could not parse message");
    }
    return 1;
  } else {
    if (bytecount == 0) {
      log(WARNING, TAG, "peer disconnected");
    } else {
      perror("recv");
    }
    return -1;
  }
}

void manage_connexion(connection_handler &handler) {
  int connected = 1;
  fd_set read_set;

  FD_ZERO(&read_set);
  FD_SET(handler.socket, &read_set);
  while (connected) {
    fd_set rsd = read_set;
    log(DEBUG, TAG, "waiting data...");
    int numReady = select(handler.socket + 1, &rsd, NULL, NULL, 0);
    if (numReady > 0) {
      if(receive_data(handler.socket) == -1){
        connected = 0;
      }
    } else {
      perror("select");
      connected = 0;
    }
  }
  log(INFO, TAG, "exiting");
}

/*
 * Launch the agent and make it listen on the local abstract socket
 * specified as the first command line parameter.
 */
int main(int argc, char **argv) {
  char *socket_name = argv[1];
  int agent_sock;
  struct sockaddr_un client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  int client_sock;

  agent_sock = create_socket(socket_name);
  if (agent_sock == -1) {
    log(ERROR, "main", "Error creating socket: %s", socket_name);
    exit(1);
  }
  if (listen(agent_sock, 1) == -1) {
    perror("listening socket");
    exit(1);
  }
  log(INFO, "main", "waiting for a connection on localabstract:%s\n",
      socket_name);
  while (true) {
    if ((client_sock = accept(agent_sock, (sockaddr *)&client_addr,
                              &client_addr_len)) != -1) {
      log(INFO, "main", "received connection on %s\n", socket_name);
      connection_handler h;
      h.socket = client_sock;
      h.endpoint = socket_name;
      TAG = socket_name;
      manage_connexion(h);
    } else {
      perror("accepting socket");
    }
  }
}
