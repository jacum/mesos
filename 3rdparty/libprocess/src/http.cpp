#include <arpa/inet.h>

#include <cstring>
#include <deque>
#include <iostream>
#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/io.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "decoder.hpp"

using std::deque;
using std::string;

using process::http::Request;
using process::http::Response;

namespace process {

namespace http {

hashmap<uint16_t, string> statuses;

namespace internal {

Future<Response> decode(const string& buffer)
{
  ResponseDecoder decoder;
  deque<Response*> responses = decoder.decode(buffer.c_str(), buffer.length());

  if (decoder.failed() || responses.empty()) {
    for (size_t i = 0; i < responses.size(); ++i) {
      delete responses[i];
    }
    return Failure("Failed to decode HTTP response:\n" + buffer + "\n");
  } else if (responses.size() > 1) {
    PLOG(ERROR) << "Received more than 1 HTTP Response";
  }

  Response response = *responses[0];
  for (size_t i = 0; i < responses.size(); ++i) {
    delete responses[i];
  }

  return response;
}


Future<Response> request(
    const UPID& upid,
    const string& method,
    const Option<string>& path,
    const Option<string>& query,
    const Option<string>& body,
    const Option<string>& contentType)
{
  Try<int> socket = process::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

  if (socket.isError()) {
    return Failure("Failed to create socket: " + socket.error());
  }

  int s = socket.get();

  Try<Nothing> cloexec = os::cloexec(s);
  if (!cloexec.isSome()) {
    os::close(s);
    return Failure("Failed to cloexec: " + cloexec.error());
  }

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(upid.port);
  addr.sin_addr.s_addr = upid.ip;

  if (connect(s, (sockaddr*) &addr, sizeof(addr)) < 0) {
    os::close(s);
    return Failure(string("Failed to connect: ") + strerror(errno));
  }

  std::ostringstream out;

  out << method << " /" << upid.id;

  if (path.isSome()) {
    out << "/" << path.get();
  }

  if (query.isSome()) {
    out << "?" << query.get();
  }

  out << " HTTP/1.1\r\n";

  // Call inet_ntop since inet_ntoa is not thread-safe!
  char ip[INET_ADDRSTRLEN];
  PCHECK(inet_ntop(AF_INET, (in_addr *) &upid.ip, ip, INET_ADDRSTRLEN) != NULL);

  out << "Host: " << ip << ":" << upid.port << "\r\n"
      << "Connection: close\r\n";

  if (body.isNone() && contentType.isSome()) {
    os::close(s);
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  if (contentType.isSome()) {
    out << "Content-Type: " << contentType.get() << "\r\n";
  }

  if (body.isNone()) {
    out << "\r\n";
  } else {
    out << "Content-Length: " << body.get().length() << "\r\n"
        << "\r\n"
        << body.get();
  }

  Try<Nothing> nonblock = os::nonblock(s);
  if (!nonblock.isSome()) {
    os::close(s);
    return Failure("Failed to set nonblock: " + nonblock.error());
  }

  // Need to disambiguate the io::read we want when binding below.
  Future<string> (*read)(int) = io::read;

  return io::write(s, out.str())
    .then(lambda::bind(read, s))
    .then(lambda::bind(&internal::decode, lambda::_1))
    .onAny(lambda::bind(&os::close, s));
}


} // namespace internal {


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query)
{
  return internal::request(upid, "GET", path, query, None(), None());
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& body,
    const Option<string>& contentType)
{
  return internal::request(upid, "POST", path, None(), body, contentType);
}


} // namespace http {
} // namespace process {
