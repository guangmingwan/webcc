#include "webcc/http_connection.h"

#include <utility>  // for move()
#include <vector>

#include "boost/asio/write.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"

#include "webcc/http_request_handler.h"
#include "webcc/logger.h"

namespace webcc {

HttpConnection::HttpConnection(boost::asio::ip::tcp::socket socket,
                               HttpRequestHandler* handler)
    : socket_(std::move(socket)),
      buffer_(kBufferSize),
      request_handler_(handler),
      request_parser_(&request_) {
}

void HttpConnection::Start() {
  AsyncRead();
}

void HttpConnection::Close() {
  boost::system::error_code ignored_ec;
  socket_.close(ignored_ec);
}

void HttpConnection::SetResponseContent(std::string&& content,
                                        const std::string& type) {
  response_.SetContent(std::move(content), true);
  response_.SetContentType(type);
}

void HttpConnection::SendResponse(HttpStatus::Enum status) {
  response_.set_status(status);
  response_.UpdateStartLine();
  AsyncWrite();
}

void HttpConnection::AsyncRead() {
  socket_.async_read_some(boost::asio::buffer(buffer_),
                          std::bind(&HttpConnection::ReadHandler,
                                    shared_from_this(),
                                    std::placeholders::_1,
                                    std::placeholders::_2));
}

void HttpConnection::ReadHandler(boost::system::error_code ec,
                                 std::size_t length) {
  if (ec) {
    if (ec != boost::asio::error::operation_aborted) {
      Close();
    }
    return;
  }

  if (!request_parser_.Parse(buffer_.data(), length)) {
    // Bad request.
    LOG_ERRO("Failed to parse HTTP request.");
    response_ = HttpResponse::Fault(HttpStatus::kBadRequest);
    AsyncWrite();
    return;
  }

  if (!request_parser_.finished()) {
    // Continue to read the request.
    AsyncRead();
    return;
  }

  LOG_VERB("HTTP request:\n%s", request_.Dump(4, "> ").c_str());

  // Enqueue this connection.
  // Some worker thread will handle it later.
  request_handler_->Enqueue(shared_from_this());
}

void HttpConnection::AsyncWrite() {
  LOG_VERB("HTTP response:\n%s", response_.Dump(4, "> ").c_str());

  boost::asio::async_write(socket_,
                           response_.ToBuffers(),
                           std::bind(&HttpConnection::WriteHandler,
                                     shared_from_this(),
                                     std::placeholders::_1,
                                     std::placeholders::_2));
}

// NOTE:
// This write handler will be called from main thread (the thread calling
// io_context.run), even though AsyncWrite() is invoked by worker threads.
// This is ensured by Asio.
void HttpConnection::WriteHandler(boost::system::error_code ec,
                                  std::size_t length) {
  if (!ec) {
    LOG_INFO("Response has been sent back, length: %u.", length);

    // Initiate graceful connection closure.
    boost::system::error_code ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

  } else {
    LOG_ERRO("Sending response error: %s", ec.message().c_str());

    if (ec != boost::asio::error::operation_aborted) {
      Close();
    }
  }
}

}  // namespace webcc