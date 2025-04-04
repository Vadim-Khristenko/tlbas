//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2025
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpStatConnection.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/Promise.h"

namespace telegram_bot_api {

void HttpStatConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                               td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);
  
  if (http_query->type_ != td::HttpQuery::Type::Get) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error,
                 td::Status::Error(405, "Method Not Allowed: closing"));
    return;
  }
  
  int format_type = 0;
  td::string format_type_str;
  for (const auto& arg : http_query->args_) {
    if (arg.first == "format") {
      format_type_str = arg.second.str();
      break;
    }
  }
  
  if (!format_type_str.empty()) {
    if (format_type_str == "text" || format_type_str == "txt" || format_type_str == "plain") {
      format_type = 0;
    } else if (format_type_str == "html" || format_type_str == "web") {
      format_type = 1;
    } else if (format_type_str == "json") {
      format_type = 2;
    } else {
      send_closure(connection_.release(), &td::HttpInboundConnection::write_error,
                   td::Status::Error(400, "Bad Request: invalid format specified"));
      return;
    }
  }
  format_type_ = format_type;

  auto promise = td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::BufferSlice> result) {
    send_closure(actor_id, &HttpStatConnection::on_result, std::move(result));
  });
  
  td::vector<std::pair<td::string, td::string>> args;
  for (const auto& arg_pair : http_query->args_) {
    args.emplace_back(arg_pair.first.str(), arg_pair.second.str());
  }
  
  send_closure(client_manager_, 
               static_cast<void (ClientManager::*)(td::Promise<td::BufferSlice>, td::vector<std::pair<td::string, td::string>>, int)>(&ClientManager::get_stats),
               std::move(promise), std::move(args), format_type);
}

void HttpStatConnection::on_result(td::Result<td::BufferSlice> result) {
  if (result.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error,
                 td::Status::Error(500, "Internal Server Error: closing"));
    return;
  }

  auto content = result.move_as_ok();
  td::HttpHeaderCreator hc;
  hc.init_status_line(200);
  hc.set_keep_alive();
  
  if (format_type_ == 0) {
    hc.set_content_type("text/plain");
  } else if (format_type_ == 1) {
    hc.set_content_type("text/html");
  } else if (format_type_ == 2) {
    hc.set_content_type("application/json");
  }
  
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  if (r_header.is_error()) {
    send_closure(connection_.release(), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(connection_.release(), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
