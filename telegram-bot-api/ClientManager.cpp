//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2025
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/ClientManager.h"

#include "telegram-bot-api/ClientParameters.h"
#include "telegram-bot-api/WebhookActor.h"

#include "td/telegram/ClientActor.h"
#include "td/telegram/td_api.h"

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/ConcurrentBinlog.h"
#include "td/db/BinlogKeyValue.h"
#include "td/db/DbKey.h"
#include "td/db/TQueue.h"

#include "td/net/HttpFile.h"

#include "td/actor/MultiPromise.h"

#include "td/utils/common.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/thread.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StackAllocator.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"

#include "memprof/memprof.h"

#include <algorithm>
#include <atomic>
#include <tuple>

namespace telegram_bot_api {

void ClientManager::close(td::Promise<td::Unit> &&promise) {
  close_promises_.push_back(std::move(promise));
  if (close_flag_) {
    return;
  }

  close_flag_ = true;
  watchdog_id_.reset();
  dump_statistics();
  auto ids = clients_.ids();
  for (auto id : ids) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);
    send_closure(client_info->client_, &Client::close);
  }
  if (ids.empty()) {
    close_db();
  }
}

void ClientManager::send(PromisedQueryPtr query) {
  if (close_flag_) {
    // automatically send 429
    return;
  }

  td::string token = query->token().str();
  if (token[0] == '0' || token.size() > 80u || token.find('/') != td::string::npos ||
      token.find(':') == td::string::npos) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }
  auto r_user_id = td::to_integer_safe<td::int64>(query->token().substr(0, token.find(':')));
  if (r_user_id.is_error() || !token_range_(r_user_id.ok())) {
    return fail_query(421, "Misdirected Request: unallowed token specified", std::move(query));
  }
  auto user_id = r_user_id.ok();
  if (user_id <= 0 || user_id >= (static_cast<td::int64>(1) << 54)) {
    return fail_query(401, "Unauthorized: invalid token specified", std::move(query));
  }

  if (query->is_test_dc()) {
    token += "/test";
  }

  auto id_it = token_to_id_.find(token);
  if (id_it == token_to_id_.end()) {
    auto method = query->method();
    if (method == "close") {
      return fail_query(400, "Bad Request: the bot has already been closed", std::move(query));
    }

    td::string ip_address = query->get_peer_ip_address();
    if (!ip_address.empty()) {
      td::IPAddress tmp;
      tmp.init_host_port(ip_address, 0).ignore();
      tmp.clear_ipv6_interface();
      if (tmp.is_valid()) {
        ip_address = tmp.get_ip_str().str();
      }
    }
    LOG(DEBUG) << "Receive incoming query for new bot " << token << " from " << ip_address;
    if (!ip_address.empty()) {
      LOG(DEBUG) << "Check Client creation flood control for IP address " << ip_address;
      auto res = flood_controls_.emplace(std::move(ip_address), td::FloodControlFast());
      auto &flood_control = res.first->second;
      if (res.second) {
        flood_control.add_limit(60, 20);        // 20 in a minute
        flood_control.add_limit(60 * 60, 600);  // 600 in an hour
      }
      auto now = td::Time::now();
      auto wakeup_at = flood_control.get_wakeup_at();
      if (wakeup_at > now) {
        LOG(INFO) << "Failed to create Client from IP address " << ip_address;
        return query->set_retry_after_error(static_cast<int>(wakeup_at - now) + 1);
      }
      flood_control.add_event(now);
    }
    auto tqueue_id = get_tqueue_id(user_id, query->is_test_dc());
    if (active_client_count_.count(tqueue_id) != 0) {
      // return query->set_retry_after_error(1);
    }

    auto id =
        clients_.create(ClientInfo{BotStatActor(stat_.actor_id(&stat_)), token, tqueue_id, td::ActorOwn<Client>()});
    auto *client_info = clients_.get(id);
    client_info->client_ = td::create_actor<Client>(PSLICE() << "Client/" << token, actor_shared(this, id),
                                                    query->token().str(), query->is_test_dc(), tqueue_id, parameters_,
                                                    client_info->stat_.actor_id(&client_info->stat_));

    if (method != "deletewebhook" && method != "setwebhook") {
      auto bot_token_with_dc = PSTRING() << query->token() << (query->is_test_dc() ? ":T" : "");
      auto webhook_info = parameters_->shared_data_->webhook_db_->get(bot_token_with_dc);
      if (!webhook_info.empty()) {
        send_closure(client_info->client_, &Client::send,
                     get_webhook_restore_query(bot_token_with_dc, webhook_info, parameters_->shared_data_));
      }
    }

    std::tie(id_it, std::ignore) = token_to_id_.emplace(token, id);
  }
  send_closure(clients_.get(id_it->second)->client_, &Client::send,
               std::move(query));  // will send 429 if the client is already closed
}

ClientManager::TopClients ClientManager::get_top_clients(std::size_t max_count, td::Slice token_filter) {
  auto now = td::Time::now();
  TopClients result;
  td::vector<std::pair<td::int64, td::uint64>> top_client_ids;
  for (auto id : clients_.ids()) {
    auto *client_info = clients_.get(id);
    CHECK(client_info);

    if (client_info->stat_.is_active(now)) {
      result.active_count++;
    }

    if (!td::begins_with(client_info->token_, token_filter)) {
      continue;
    }

    auto score = static_cast<td::int64>(client_info->stat_.get_score(now) * -1e9);
    if (score == 0 && top_client_ids.size() >= max_count) {
      continue;
    }
    top_client_ids.emplace_back(score, id);
  }
  if (top_client_ids.size() < max_count) {
    max_count = top_client_ids.size();
  }
  std::partial_sort(top_client_ids.begin(), top_client_ids.begin() + max_count, top_client_ids.end());
  result.top_client_ids.reserve(max_count);
  for (std::size_t i = 0; i < max_count; i++) {
    result.top_client_ids.push_back(top_client_ids[i].second);
  }
  return result;
}

void ClientManager::get_stats(td::Promise<td::BufferSlice> promise,
                              td::vector<std::pair<td::string, td::string>> args) {
  int format_type = 0;
  for (auto &arg : args) {
    if (arg.first == "format") {
      if (arg.second == "html") {
        format_type = 1;
      } else if (arg.second == "json") {
        format_type = 2;
      }
    }
  }
  
  get_stats(std::move(promise), std::move(args), format_type);
}

void ClientManager::get_stats(td::Promise<td::BufferSlice> promise,
                              td::vector<std::pair<td::string, td::string>> args,
                              int format_type) {
  if (close_flag_) {
    promise.set_value(td::BufferSlice("Closing"));
    return;
  }

  td::Slice id_filter;
  int new_verbosity_level = -1;
  td::string tag;
  
  for (auto &arg : args) {
    if (arg.first == "id") {
      id_filter = arg.second;
    } else if (arg.first == "v") {
      auto r_new_verbosity_level = td::to_integer_safe<int>(arg.second);
      if (r_new_verbosity_level.is_ok()) {
        new_verbosity_level = r_new_verbosity_level.ok();
      }
    } else if (arg.first == "tag") {
      tag = arg.second;
    }
  }
  
  if (new_verbosity_level > 0) {
    if (tag.empty()) {
      parameters_->shared_data_->next_verbosity_level_ = new_verbosity_level;
    } else {
      td::ClientActor::execute(td::td_api::make_object<td::td_api::setLogTagVerbosityLevel>(tag, new_verbosity_level));
    }
  }

  auto now = td::Time::now();
  auto stats_data = collect_stats_data(now, id_filter);
  
  if (format_type == 0) {
    promise.set_value(format_stats_as_text(stats_data));
  } else if (format_type == 1) {
    promise.set_value(format_stats_as_html(stats_data));
  } else if (format_type == 2) {
    promise.set_value(format_stats_as_json(stats_data));
  } else {
    promise.set_error(td::Status::Error(400, "Bad Request: invalid format specified"));
  }
}

ClientManager::ServerStats ClientManager::collect_stats_data(double now, td::Slice id_filter) {
  ServerStats stats;
  auto top_clients = get_top_clients(50, id_filter);
  
  if (id_filter.empty()) {
    stats.uptime = now - parameters_->start_time_;
    stats.bot_count = clients_.size();
    stats.active_bot_count = top_clients.active_count;
    
    auto r_mem_stat = td::mem_stat();
    if (r_mem_stat.is_ok()) {
      auto mem_stat = r_mem_stat.move_as_ok();
      stats.memory.resident_size = mem_stat.resident_size_;
      stats.memory.virtual_size = mem_stat.virtual_size_;
      stats.memory.resident_size_peak = mem_stat.resident_size_peak_;
      stats.memory.virtual_size_peak = mem_stat.virtual_size_peak_;
    } else {
      LOG(INFO) << "Failed to get memory statistics: " << r_mem_stat.error();
    }
    
    stats.cpu_stats = ServerCpuStat::instance().as_vector(now);
    stats.buffer_memory = td::BufferAllocator::get_buffer_mem();
    stats.active_webhook_connections = WebhookActor::get_total_connection_count();
    stats.active_requests = parameters_->shared_data_->query_count_.load(std::memory_order_relaxed);
    stats.active_network_queries = td::get_pending_network_query_count(*parameters_->net_query_stats_);
    stats.server_stats = stat_.as_vector(now);
  }
  
  for (auto top_client_id : top_clients.top_client_ids) {
    auto *client_info = clients_.get(top_client_id);
    CHECK(client_info);
    
    ServerStats::BotStats bot_stats;
    auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
    
    bot_stats.id = td::to_integer<td::int64>(bot_info.id_);
    bot_stats.uptime = now - bot_info.start_time_;
    bot_stats.token = bot_info.token_;
    bot_stats.username = bot_info.username_;
    bot_stats.active_request_count = client_info->stat_.get_active_request_count();
    bot_stats.active_file_upload_bytes = client_info->stat_.get_active_file_upload_bytes();
    bot_stats.active_file_upload_count = client_info->stat_.get_active_file_upload_count();
    bot_stats.webhook = bot_info.webhook_;
    bot_stats.has_webhook_certificate = bot_info.has_webhook_certificate_;
    bot_stats.webhook_max_connections = bot_info.webhook_max_connections_;
    bot_stats.head_update_id = bot_info.head_update_id_;
    bot_stats.tail_update_id = bot_info.tail_update_id_;
    bot_stats.pending_update_count = bot_info.pending_update_count_;
    bot_stats.stats = client_info->stat_.as_vector(now);
    
    stats.bots.push_back(std::move(bot_stats));
  }
  
  return stats;
}

td::BufferSlice ClientManager::format_stats_as_text(const ServerStats& stats) {
  size_t buf_size = 1 << 14;
  auto buf = td::StackAllocator::alloc(buf_size);
  td::StringBuilder sb(buf.as_slice());
  
  sb << BotStatActor::get_description() << '\n';
  
  if (stats.bots.empty() || stats.bot_count != 0) {
    sb << "uptime\t" << stats.uptime << '\n';
    sb << "bot_count\t" << stats.bot_count << '\n';
    sb << "active_bot_count\t" << stats.active_bot_count << '\n';
    
    if (stats.memory.resident_size > 0) {
      sb << "rss\t" << td::format::as_size(stats.memory.resident_size) << '\n';
      sb << "vm\t" << td::format::as_size(stats.memory.virtual_size) << '\n';
      sb << "rss_peak\t" << td::format::as_size(stats.memory.resident_size_peak) << '\n';
      sb << "vm_peak\t" << td::format::as_size(stats.memory.virtual_size_peak) << '\n';
    }
    
    for (const auto& stat : stats.cpu_stats) {
      sb << stat.key_ << "\t" << stat.value_ << '\n';
    }
    
    sb << "buffer_memory\t" << td::format::as_size(stats.buffer_memory) << '\n';
    sb << "active_webhook_connections\t" << stats.active_webhook_connections << '\n';
    sb << "active_requests\t" << stats.active_requests << '\n';
    sb << "active_network_queries\t" << stats.active_network_queries << '\n';
    
    for (const auto& stat : stats.server_stats) {
      sb << stat.key_ << "\t" << stat.value_ << '\n';
    }
  }
  
  for (const auto& bot : stats.bots) {
    sb << '\n';
    sb << "id\t" << bot.id << '\n';
    sb << "uptime\t" << bot.uptime << '\n';
    td::string masked_token = bot.token.substr(0, 6) + "..." + bot.token.substr(bot.token.size() - 4);
    sb << "token\t" << masked_token << '\n';
    sb << "username\t" << bot.username << '\n';
    
    if (bot.active_request_count != 0) {
      sb << "active_request_count\t" << bot.active_request_count << '\n';
    }
    if (bot.active_file_upload_bytes != 0) {
      sb << "active_file_upload_bytes\t" << bot.active_file_upload_bytes << '\n';
    }
    if (bot.active_file_upload_count != 0) {
      sb << "active_file_upload_count\t" << bot.active_file_upload_count << '\n';
    }
    
    if (!bot.webhook.empty()) {
      sb << "webhook\t" << bot.webhook << '\n';
      if (bot.has_webhook_certificate) {
        sb << "has_custom_certificate\t" << bot.has_webhook_certificate << '\n';
      }
      if (bot.webhook_max_connections != parameters_->default_max_webhook_connections_) {
        sb << "webhook_max_connections\t" << bot.webhook_max_connections << '\n';
      }
    }
    
    sb << "head_update_id\t" << bot.head_update_id << '\n';
    if (bot.pending_update_count != 0) {
      sb << "tail_update_id\t" << bot.tail_update_id << '\n';
      sb << "pending_update_count\t" << bot.pending_update_count << '\n';
    }
    
    for (const auto& stat : bot.stats) {
      if (stat.key_ == "update_count" || stat.key_ == "request_count") {
        sb << stat.key_ << "/sec\t" << stat.value_ << '\n';
      }
    }
    
    if (sb.is_error()) {
      break;
    }
  }
  
  return td::BufferSlice(sb.as_cslice());
}

td::BufferSlice ClientManager::format_stats_as_json(const ServerStats& stats) {
  td::string json = "{";
  json += "\"uptime\":" + td::to_string(stats.uptime) + ",";
  json += "\"bot_count\":" + td::to_string(stats.bot_count) + ",";
  json += "\"active_bot_count\":" + td::to_string(stats.active_bot_count) + ",";
  json += "\"memory\":{";
  json += "\"rss\":" + td::to_string(stats.memory.resident_size) + ",";
  json += "\"vm\":" + td::to_string(stats.memory.virtual_size) + ",";
  json += "\"rss_peak\":" + td::to_string(stats.memory.resident_size_peak) + ",";
  json += "\"vm_peak\":" + td::to_string(stats.memory.virtual_size_peak) + ",";
  json += "\"buffer_memory\":" + td::to_string(stats.buffer_memory) + "},";

  json += "\"cpu_stats\":{";
  for (const auto& stat : stats.cpu_stats) {
    td::string value_str = stat.value_;
    td::vector<td::string> values;
    td::Parser parser(value_str);
    while (!parser.empty()) {
      auto value = parser.read_word();
        if (!value.empty()) {
          values.push_back(value.str());
        }
      }
    
    td::string values_json = "";
    for (size_t i = 0; i < values.size(); i++) {
      values_json += "\"" + values[i] + "\"";
      if (i < values.size() - 1) {
        values_json += ",";
      }
    }
    
    json += "\"" + escape_json_string(stat.key_) + "\":[" + values_json + "]";
    if (&stat != &stats.cpu_stats.back()) {
      json += ",";
    }
  }
  json += "}";

  json += ",\"active_webhook_connections\":" + td::to_string(stats.active_webhook_connections) + ",";
  json += "\"active_requests\":" + td::to_string(stats.active_requests) + ",";
  json += "\"active_network_queries\":" + td::to_string(stats.active_network_queries) + ",";

  json += "\"server_stats\":{";
  for (const auto& stat : stats.server_stats) {
    td::string value_str = stat.value_;
    td::vector<td::string> values;
    td::Parser parser(value_str);
    while (!parser.empty()) {
      auto value = parser.read_word();
        if (!value.empty()) {
          values.push_back(value.str());
        }
      }
    
    td::string values_json = "";
    for (size_t i = 0; i < values.size(); i++) {
      values_json += values[i];
      if (i < values.size() - 1) {
        values_json += ",";
      }
    }
    
    json += "\"" + escape_json_string(stat.key_) + "\":[" + values_json + "]";
    if (&stat != &stats.server_stats.back()) {
      json += ",";
    }
  }
  json += "}";

  if (!stats.bots.empty()) {
    json += ",\"bots\":[";
    for (const auto& bot : stats.bots) {
      json += "{";
      json += "\"id\":" + td::to_string(bot.id) + ",";
      json += "\"uptime\":" + td::to_string(bot.uptime) + ",";
      td::string masked_token = bot.token.substr(0, 6) + "..." + bot.token.substr(bot.token.size() - 4);
      json += "\"token\":\"" + escape_json_string(masked_token) + "\",";
      json += "\"username\":\"" + escape_json_string(bot.username) + "\",";
      json += "\"active_request_count\":" + td::to_string(bot.active_request_count) + ",";
      json += "\"active_file_upload_bytes\":" + td::to_string(bot.active_file_upload_bytes) + ",";
      json += "\"active_file_upload_count\":" + td::to_string(bot.active_file_upload_count) + ",";
      if (!bot.webhook.empty()) {
        json += "\"webhook\":\"" + escape_json_string(bot.webhook) + "\",";
        json += "\"has_webhook_certificate\":" + std::string(bot.has_webhook_certificate ? "true" : "false") + ",";
        json += "\"webhook_max_connections\":" + td::to_string(bot.webhook_max_connections) + ",";
      }
      json += "\"head_update_id\":" + td::to_string(bot.head_update_id) + ",";
      json += "\"tail_update_id\":" + td::to_string(bot.tail_update_id) + ",";
      json += "\"pending_update_count\":" + td::to_string(bot.pending_update_count) + ",";
      
      if (!bot.stats.empty()) {
        json += "\"stats\":{";
        for (const auto& stat : bot.stats) {
          td::string value_str = stat.value_;
          td::vector<td::string> values;
          td::Parser parser(value_str);
          while (!parser.empty()) {
            auto value = parser.read_word();
              if (!value.empty()) {
                values.push_back(value.str());
              }
            }
          
          td::string values_json = "";
          for (size_t i = 0; i < values.size(); i++) {
            values_json += values[i];
            if (i < values.size() - 1) {
              values_json += ",";
            }
          }
          
          json += "\"" + escape_json_string(stat.key_) + "\":[" + values_json + "]";
          if (&stat != &bot.stats.back()) {
            json += ",";
          }
        }
        json += "}";
      }
      json += "}";
      if (&bot != &stats.bots.back()) {
        json += ",";
      }
    }
    json += "]";
  }
  json += "}";
  
  return td::BufferSlice(json);
}

td::string ClientManager::escape_json_string(const td::string& str) {
  td::string result;
  result.reserve(str.size() * 2);
  
  for (char c : str) {
    switch (c) {
      case '\"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '/': result += "\\/"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (c < 0x20) {
          result += "\\u00";
          result += td::format::hex_digit((c >> 4) & 0xf);
          result += td::format::hex_digit(c & 0xf);
        } else {
          result += c;
        }
    }
  }
  
  return result;
}

td::string ClientManager::format_size(td::uint64 size) {
  size_t buf_size = 1 << 6;
  auto buf = td::StackAllocator::alloc(buf_size);
  td::StringBuilder sb(buf.as_slice());
  sb << td::format::as_size(size);
  return sb.as_cslice().str();
}

td::BufferSlice ClientManager::format_stats_as_html(const ServerStats& stats) {
  td::string html = "<!DOCTYPE html>\n"
                    "<html>\n"
                    "<head>\n"
                    "  <title>Telegram Bot API Server Statistics</title>\n"
                    "  <meta charset=\"utf-8\">\n"
                    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                    "  <style>\n"
                    "    :root {\n"
                    "      --primary: #0088cc;\n"
                    "      --primary-light: #e7f5fb;\n"
                    "      --secondary: #666;\n"
                    "      --bg-light: #f8f9fa;\n"
                    "      --border: #e0e0e0;\n"
                    "      --box-shadow: 0 2px 10px rgba(0,0,0,0.1);\n"
                    "      --text-color: #333;\n"
                    "      --bg-color: #fafafa;\n"
                    "      --card-bg: white;\n"
                    "      --header-bg: var(--primary);\n"
                    "      --header-text: white;\n"
                    "      --copyable-bg: #e7f5fb;\n"
                    "      --copyable-success: #8fd4ff;\n"
                    "    }\n"
                    "    \n"
                    "    body.dark-mode {\n"
                    "      --primary: #1e88e5;\n"
                    "      --primary-light: #1e3a5f;\n"
                    "      --secondary: #aaa;\n"
                    "      --bg-light: #242424;\n"
                    "      --border: #444;\n"
                    "      --box-shadow: 0 2px 10px rgba(0,0,0,0.3);\n"
                    "      --text-color: #eee;\n"
                    "      --bg-color: #121212;\n"
                    "      --card-bg: #1e1e1e;\n"
                    "      --header-bg: #223b5c;\n"
                    "      --header-text: #e6e6e6;\n"
                    "      --copyable-bg: #1e3a5f;\n"
                    "      --copyable-success: #3a6ea5;\n"
                    "    }\n"
                    "\n"
                    "    @media (prefers-color-scheme: dark) {\n"
                    "      :root.system-theme {\n"
                    "        --primary: #1e88e5;\n"
                    "        --primary-light: #1e3a5f;\n"
                    "        --secondary: #aaa;\n"
                    "        --bg-light: #242424;\n"
                    "        --border: #444;\n"
                    "        --box-shadow: 0 2px 10px rgba(0,0,0,0.3);\n"
                    "        --text-color: #eee;\n"
                    "        --bg-color: #121212;\n"
                    "        --card-bg: #1e1e1e;\n"
                    "        --header-bg: #223b5c;\n"
                    "        --header-text: #e6e6e6;\n"
                    "        --copyable-bg: #1e3a5f;\n"
                    "        --copyable-success: #3a6ea5;\n"
                    "      }\n"
                    "    }\n"
                    "    \n"
                    "    body {\n"
                    "      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;\n"
                    "      margin: 0;\n"
                    "      padding: 20px;\n"
                    "      color: var(--text-color);\n"
                    "      background-color: var(--bg-color);\n"
                    "      transition: background-color 0.3s ease, color 0.3s ease;\n"
                    "    }\n"
                    "    h1, h2, h3 {\n"
                    "      color: var(--primary);\n"
                    "      margin-top: 0;\n"
                    "      transition: color 0.3s ease;\n"
                    "    }\n"
                    "    .content-wrapper {\n"
                    "      max-width: 1400px;\n"
                    "      margin: 0 auto;\n"
                    "      padding: 0 10px;\n"
                    "    }\n"
                    "    .stats-container {\n"
                    "      display: flex;\n"
                    "      flex-direction: column;\n"
                    "      gap: 20px;\n"
                    "      margin-bottom: 20px;\n"
                    "    }\n"
                    "    .stats-row {\n"
                    "      display: grid;\n"
                    "      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));\n"
                    "      gap: 20px;\n"
                    "    }\n"
                    "    .stats-row-wide {\n"
                    "      grid-column: 1 / -1;\n"
                    "    }\n"
                    "    .stats-box {\n"
                    "      background: var(--card-bg);\n"
                    "      border-radius: 10px;\n"
                    "      padding: 15px;\n"
                    "      width: 100%;\n"
                    "      box-shadow: var(--box-shadow);\n"
                    "      border: 1px solid var(--border);\n"
                    "      overflow: hidden;\n"
                    "      box-sizing: border-box;\n"
                    "      transition: background-color 0.3s ease, border-color 0.3s ease, box-shadow 0.3s ease;\n"
                    "    }\n"
                    "    .stat-row {\n"
                    "      display: flex;\n"
                    "      justify-content: space-between;\n"
                    "      margin-bottom: 12px;\n"
                    "      padding-bottom: 12px;\n"
                    "      border-bottom: 1px solid var(--border);\n"
                    "      flex-wrap: wrap;\n"
                    "      transition: border-color 0.3s ease;\n"
                    "    }\n"
                    "    .stat-row:last-child {\n"
                    "      border-bottom: none;\n"
                    "      margin-bottom: 0;\n"
                    "      padding-bottom: 0;\n"
                    "    }\n"
                    "    .stat-label {\n"
                    "      color: var(--secondary);\n"
                    "      margin-right: 10px;\n"
                    "      flex: 1;\n"
                    "      min-width: 120px;\n"
                    "      transition: color 0.3s ease;\n"
                    "    }\n"
                    "    .stat-value {\n"
                    "      font-weight: 600;\n"
                    "      text-align: right;\n"
                    "      flex: 2;\n"
                    "      word-break: break-word;\n"
                    "      transition: color 0.3s ease;\n"
                    "    }\n"
                    "    .copyable {\n"
                    "      cursor: pointer;\n"
                    "      position: relative;\n"
                    "      padding: 2px 8px;\n"
                    "      border-radius: 4px;\n"
                    "      background-color: var(--copyable-bg);\n"
                    "      transition: background-color 0.3s ease;\n"
                    "      display: inline-block;\n"
                    "      max-width: 100%;\n"
                    "      overflow: hidden;\n"
                    "      text-overflow: ellipsis;\n"
                    "    }\n"
                    "    .copyable:hover {\n"
                    "      background-color: var(--primary-light);\n"
                    "      filter: brightness(1.1);\n"
                    "    }\n"
                    "    .copyable::after {\n"
                    "      content: 'Copy';\n"
                    "      position: absolute;\n"
                    "      top: -25px;\n"
                    "      left: 50%;\n"
                    "      transform: translateX(-50%);\n"
                    "      padding: 3px 8px;\n"
                    "      border-radius: 3px;\n"
                    "      background: rgba(0,0,0,0.7);\n"
                    "      color: white;\n"
                    "      font-size: 12px;\n"
                    "      opacity: 0;\n"
                    "      pointer-events: none;\n"
                    "      transition: opacity 0.2s;\n"
                    "      z-index: 10;\n"
                    "    }\n"
                    "    .copyable:hover::after {\n"
                    "      opacity: 1;\n"
                    "    }\n"
                    "    .bot-container {\n"
                    "      margin-top: 40px;\n"
                    "    }\n"
                    "    .bot-card {\n"
                    "      margin-bottom: 30px;\n"
                    "      border-radius: 10px;\n"
                    "      overflow: hidden;\n"
                    "      box-shadow: var(--box-shadow);\n"
                    "      transition: box-shadow 0.3s ease;\n"
                    "    }\n"
                    "    .bot-header {\n"
                    "      background: var(--header-bg);\n"
                    "      color: var(--header-text);\n"
                    "      padding: 15px 20px;\n"
                    "      display: flex;\n"
                    "      justify-content: space-between;\n"
                    "      align-items: center;\n"
                    "      flex-wrap: wrap;\n"
                    "      transition: background-color 0.3s ease, color 0.3s ease;\n"
                    "    }\n"
                    "    .bot-header h2 {\n"
                    "      color: var(--header-text);\n"
                    "      margin: 0;\n"
                    "      word-break: break-word;\n"
                    "      transition: color 0.3s ease;\n"
                    "    }\n"
                    "    .bot-body {\n"
                    "      background: var(--card-bg);\n"
                    "      padding: 20px;\n"
                    "      transition: background-color 0.3s ease;\n"
                    "    }\n"
                    "    .stats-table {\n"
                    "      width: 100%;\n"
                    "      border-collapse: collapse;\n"
                    "      margin-bottom: 15px;\n"
                    "      overflow-x: auto;\n"
                    "      display: block;\n"
                    "    }\n"
                    "    .stats-table thead, .stats-table tbody, .stats-table tr {\n"
                    "      display: table;\n"
                    "      width: 100%;\n"
                    "      table-layout: fixed;\n"
                    "    }\n"
                    "    .stats-table th, .stats-table td {\n"
                    "      padding: 10px;\n"
                    "      text-align: left;\n"
                    "      border-bottom: 1px solid var(--border);\n"
                    "      word-break: break-word;\n"
                    "      transition: border-color 0.3s ease, color 0.3s ease;\n"
                    "    }\n"
                    "    .stats-table th {\n"
                    "      color: var(--secondary);\n"
                    "      font-weight: 500;\n"
                    "    }\n"
                    "    .stats-table td:not(:first-child) {\n"
                    "      text-align: center;\n"
                    "    }\n"
                    "    .stats-table th:not(:first-child) {\n"
                    "      text-align: center;\n"
                    "    }\n"
                    "    .stats-table tr:last-child td {\n"
                    "      border-bottom: none;\n"
                    "    }\n"
                    "    .theme-switch {\n"
                    "      position: fixed;\n"
                    "      top: 20px;\n"
                    "      right: 20px;\n"
                    "      width: 40px;\n"
                    "      height: 40px;\n"
                    "      border-radius: 50%;\n"
                    "      background-color: var(--primary);\n"
                    "      color: white;\n"
                    "      display: flex;\n"
                    "      align-items: center;\n"
                    "      justify-content: center;\n"
                    "      cursor: pointer;\n"
                    "      box-shadow: var(--box-shadow);\n"
                    "      z-index: 100;\n"
                    "      transition: background-color 0.3s ease, box-shadow 0.3s ease;\n"
                    "    }\n"
                    "    .theme-switch i {\n"
                    "      font-size: 20px;\n"
                    "    }\n"
                    "    .theme-menu {\n"
                    "      position: fixed;\n"
                    "      top: 70px;\n"
                    "      right: 20px;\n"
                    "      background-color: var(--card-bg);\n"
                    "      border-radius: 10px;\n"
                    "      box-shadow: var(--box-shadow);\n"
                    "      padding: 10px 0;\n"
                    "      z-index: 99;\n"
                    "      display: none;\n"
                    "      transition: background-color 0.3s ease, box-shadow 0.3s ease;\n"
                    "    }\n"
                    "    .theme-menu.visible {\n"
                    "      display: block;\n"
                    "    }\n"
                    "    .theme-menu-item {\n"
                    "      padding: 8px 15px;\n"
                    "      cursor: pointer;\n"
                    "      white-space: nowrap;\n"
                    "      display: flex;\n"
                    "      align-items: center;\n"
                    "      transition: background-color 0.2s;\n"
                    "    }\n"
                    "    .theme-menu-item:hover {\n"
                    "      background-color: var(--bg-light);\n"
                    "    }\n"
                    "    .theme-menu-item.active {\n"
                    "      color: var(--primary);\n"
                    "      font-weight: bold;\n"
                    "    }\n"
                    "    .theme-menu-item i {\n"
                    "      margin-right: 8px;\n"
                    "      font-size: 18px;\n"
                    "    }\n"
                    "    @media screen and (max-width: 768px) {\n"
                    "      .stats-row {\n"
                    "        grid-template-columns: 1fr;\n"
                    "      }\n"
                    "      .content-wrapper {\n"
                    "        padding: 0 5px;\n"
                    "      }\n"
                    "      body {\n"
                    "        padding: 10px;\n"
                    "      }\n"
                    "      .bot-body {\n"
                    "        padding: 15px 10px;\n"
                    "      }\n"
                    "    }\n"
                    "    @media screen and (max-width: 480px) {\n"
                    "      .stat-row {\n"
                    "        flex-direction: column;\n"
                    "        align-items: flex-start;\n"
                    "      }\n"
                    "      .stat-value {\n"
                    "        text-align: left;\n"
                    "        margin-top: 5px;\n"
                    "        width: 100%;\n"
                    "      }\n"
                    "      .theme-switch {\n"
                    "        top: 10px;\n"
                    "        right: 10px;\n"
                    "      }\n"
                    "      .theme-menu {\n"
                    "        top: 60px;\n"
                    "        right: 10px;\n"
                    "      }\n"
                    "      h1 {\n"
                    "        font-size: 1.5em;\n"
                    "        margin-top: 30px;\n"
                    "      }\n"
                    "    }\n"
                    "  </style>\n"
                    "  <link href=\"https://fonts.googleapis.com/icon?family=Material+Icons\" rel=\"stylesheet\">\n"
                    "</head>\n"
                    "<body>\n"
                    "<div class=\"theme-switch\" id=\"themeSwitch\">\n"
                    "  <i class=\"material-icons\" id=\"themeIcon\">settings</i>\n"
                    "</div>\n"
                    "<div class=\"theme-menu\" id=\"themeMenu\">\n"
                    "  <div class=\"theme-menu-item\" data-theme=\"light\">\n"
                    "    <i class=\"material-icons\">light_mode</i> Светлая тема\n"
                    "  </div>\n"
                    "  <div class=\"theme-menu-item\" data-theme=\"dark\">\n"
                    "    <i class=\"material-icons\">dark_mode</i> Темная тема\n"
                    "  </div>\n"
                    "  <div class=\"theme-menu-item\" data-theme=\"system\">\n"
                    "    <i class=\"material-icons\">settings_brightness</i> Системная тема\n"
                    "  </div>\n"
                    "</div>\n"
                    "<div class=\"content-wrapper\">\n"
                    "  <h1>Telegram Bot API Server Statistics</h1>\n";
  
  html += "  <div class='stats-container'>\n";
    
  html += "    <div class='stats-row'>\n";
    
  html += "      <div class='stats-box'>\n";
  html += "        <h2>General Info</h2>\n";
  html += "        <div class='stat-row'><span class='stat-label'>Uptime:</span> <span class='stat-value'>" + 
          std::to_string(static_cast<int>(stats.uptime)) + " seconds</span></div>\n";
  html += "        <div class='stat-row'><span class='stat-label'>Bot count:</span> <span class='stat-value'>" + 
          std::to_string(stats.bot_count) + "</span></div>\n";
  html += "        <div class='stat-row'><span class='stat-label'>Active bot count:</span> <span class='stat-value'>" + 
          std::to_string(stats.active_bot_count) + "</span></div>\n";
  html += "        <div class='stat-row'><span class='stat-label'>Active requests:</span> <span class='stat-value'>" + 
          std::to_string(stats.active_requests) + "</span></div>\n";
  html += "        <div class='stat-row'><span class='stat-label'>Active webhook connections:</span> <span class='stat-value'>" + 
          std::to_string(stats.active_webhook_connections) + "</span></div>\n";
  html += "      </div>\n";
    
  if (stats.memory.resident_size > 0) {
    html += "      <div class='stats-box'>\n";
    html += "        <h2>Memory Usage</h2>\n";
      
    html += "        <div class='stat-row'><span class='stat-label'>RSS:</span> <span class='stat-value'>" + 
            format_size(stats.memory.resident_size) + "</span></div>\n";
              
    html += "        <div class='stat-row'><span class='stat-label'>VM:</span> <span class='stat-value'>" + 
            format_size(stats.memory.virtual_size) + "</span></div>\n";
              
    html += "        <div class='stat-row'><span class='stat-label'>RSS Peak:</span> <span class='stat-value'>" + 
            format_size(stats.memory.resident_size_peak) + "</span></div>\n";
              
    html += "        <div class='stat-row'><span class='stat-label'>VM Peak:</span> <span class='stat-value'>" + 
            format_size(stats.memory.virtual_size_peak) + "</span></div>\n";
              
    html += "        <div class='stat-row'><span class='stat-label'>Buffer memory:</span> <span class='stat-value'>" + 
            format_size(stats.buffer_memory) + "</span></div>\n";
    html += "      </div>\n";
  }
    
  html += "    </div>\n";
    
  if (!stats.cpu_stats.empty()) {
    html += "    <div class='stats-row'>\n";
    html += "      <div class='stats-box stats-row-wide'>\n";
    html += "        <h2>CPU Statistics</h2>\n";
    html += "        <div class='table-container' style='overflow-x: auto;'>\n";
    html += "        <table class='stats-table'>\n";
    html += "          <thead>\n";
    html += "            <tr>\n";
    html += "              <th>Metric</th>\n";
    html += "              <th>All Time</th>\n";
    html += "              <th>5 Sec</th>\n";
    html += "              <th>1 Min</th>\n";
    html += "              <th>1 Hour</th>\n";
    html += "            </tr>\n";
    html += "          </thead>\n";
    html += "          <tbody>\n";
      
    for (const auto& stat : stats.cpu_stats) {
      td::string label = stat.key_;
      if (label == "total_cpu") {
        label = "Total CPU";
      } else if (label == "user_cpu") {
        label = "User CPU";
      } else if (label == "system_cpu") {
        label = "System CPU";
      }
        
      td::string value_str = stat.value_;
      td::vector<td::string> values;
      td::Parser parser(value_str);
      while (!parser.empty()) {
        auto value = parser.read_word();
        if (!value.empty()) {
          values.push_back(value.str());
        }
      }
        
      html += "            <tr>\n";
      html += "              <td>" + label + "</td>\n";
      for (size_t i = 0; i < values.size() && i < 4; i++) {
        html += "              <td>" + values[i] + "</td>\n";
      }

      for (size_t i = values.size(); i < 4; i++) {
        html += "              <td>-</td>\n";
      }
        
      html += "            </tr>\n";
    }
      
    html += "          </tbody>\n";
    html += "        </table>\n";
    html += "        </div>\n";
    html += "      </div>\n";
    html += "    </div>\n";
  }
    
  html += "  </div>\n";
  
  if (!stats.bots.empty()) {
    html += "  <div class='bot-container'>\n";
    html += "    <h1>Bot Statistics</h1>\n";
    
    for (const auto& bot : stats.bots) {
      html += "    <div class='bot-card'>\n";
      html += "      <div class='bot-header'>\n";
      html += "        <h2>" + (bot.username.empty() ? "Bot ID:" + std::to_string(bot.id) : "Bot @" + bot.username) + "</h2>\n";
      html += "      </div>\n";
      html += "      <div class='bot-body'>\n";
      html += "        <div class='stats-container'>\n";
      
      html += "          <div class='stats-row'>\n";
      
      html += "            <div class='stats-box'>\n";
      html += "              <h3>Bot Info</h3>\n";
      html += "              <div class='stat-row'>\n";
      html += "                <span class='stat-label'>ID:</span>\n";
      html += "                <span class='stat-value'>\n";
      html += "                  <span class='copyable' onclick='copyToClipboard(\"" + std::to_string(bot.id) + "\", event)'>" + 
              std::to_string(bot.id) + "</span>\n";
      html += "                </span>\n";
      html += "              </div>\n";
      
      if (!bot.username.empty()) {
        html += "              <div class='stat-row'>\n";
        html += "                <span class='stat-label'>Username:</span>\n";
        html += "                <span class='stat-value'>\n";
        html += "                  <span class='copyable' onclick='copyToClipboard(\"@" + bot.username + "\", event)'>" + 
                "@" + bot.username + "</span>\n";
        html += "                </span>\n";
        html += "              </div>\n";
      }
      
      html += "              <div class='stat-row'><span class='stat-label'>Uptime:</span> <span class='stat-value'>" + 
              std::to_string(static_cast<int>(bot.uptime)) + " seconds</span></div>\n";
      
      if (bot.token.size() > 10) {
        td::string masked_token = bot.token.substr(0, 6) + "..." + 
                                  bot.token.substr(bot.token.size() - 4);
        html += "              <div class='stat-row'><span class='stat-label'>Token:</span> <span class='stat-value'>" + 
                masked_token + "</span></div>\n";
      }
      
      html += "            </div>\n";
      
      html += "            <div class='stats-box'>\n";
      html += "              <h3>Updates</h3>\n";
      html += "              <div class='stat-row'><span class='stat-label'>Head update ID:</span> <span class='stat-value'>" + 
              std::to_string(bot.head_update_id) + "</span></div>\n";
      
      if (bot.pending_update_count != 0) {
        html += "              <div class='stat-row'><span class='stat-label'>Tail update ID:</span> <span class='stat-value'>" + 
                std::to_string(bot.tail_update_id) + "</span></div>\n";
        html += "              <div class='stat-row'><span class='stat-label'>Pending updates:</span> <span class='stat-value'>" + 
                std::to_string(bot.pending_update_count) + "</span></div>\n";
      }
      
      html += "            </div>\n";
      html += "          </div>\n";

      html += "          <div class='stats-row'>\n";
      html += "            <div class='stats-box stats-row-wide'>\n";
      html += "              <h3>Activity</h3>\n";
      html += "              <div class='table-container' style='overflow-x: auto;'>\n";
      html += "              <table class='stats-table'>\n";
      html += "                <thead>\n";
      html += "                  <tr>\n";
      html += "                    <th>Metric</th>\n";
      html += "                    <th>All Time</th>\n";
      html += "                    <th>5 Sec</th>\n";
      html += "                    <th>1 Min</th>\n";
      html += "                    <th>1 Hour</th>\n";
      html += "                  </tr>\n";
      html += "                </thead>\n";
      html += "                <tbody>\n";
      
      for (const auto& stat : bot.stats) {
        if (stat.key_ == "update_count" || stat.key_ == "request_count") {
          td::string value_str = stat.value_;
          td::vector<td::string> values;
          td::Parser parser(value_str);
          while (!parser.empty()) {
            auto value = parser.read_word();
            if (!value.empty()) {
              values.push_back(value.str());
            }
          }
          
          td::string label = stat.key_;
          if (label == "update_count") {
            label = "Updates";
          } else if (label == "request_count") {
            label = "Requests";
          }
          
          html += "                  <tr>\n";
          html += "                    <td>" + label + "/sec</td>\n";
          
          for (size_t i = 0; i < values.size() && i < 4; i++) {
            html += "                    <td>" + values[i] + "</td>\n";
          }
          
          for (size_t i = values.size(); i < 4; i++) {
            html += "                    <td>-</td>\n";
          }
          
          html += "                  </tr>\n";
        }
      }
      
      html += "                </tbody>\n";
      html += "              </table>\n";
      html += "              </div>\n";
      
      if (bot.active_request_count != 0) {
        html += "              <div class='stat-row'><span class='stat-label'>Active requests:</span> <span class='stat-value'>" + 
                std::to_string(bot.active_request_count) + "</span></div>\n";
      }
      
      if (bot.active_file_upload_count != 0) {
        html += "              <div class='stat-row'><span class='stat-label'>Active uploads:</span> <span class='stat-value'>" + 
                std::to_string(bot.active_file_upload_count) + "</span></div>\n";
      }
      
      if (bot.active_file_upload_bytes != 0) {
        html += "              <div class='stat-row'><span class='stat-label'>Upload bytes:</span> <span class='stat-value'>" + 
                format_size(bot.active_file_upload_bytes) + "</span></div>\n";
      }
      
      html += "            </div>\n";
      html += "          </div>\n";
      
      if (!bot.webhook.empty()) {
        html += "          <div class='stats-row'>\n";
        html += "            <div class='stats-box stats-row-wide'>\n";
        html += "              <h3>Webhook</h3>\n";
        html += "              <div class='stat-row'><span class='stat-label'>URL:</span> <span class='stat-value' style='word-break: break-all;'>" + 
                bot.webhook + "</span></div>\n";
        
        if (bot.has_webhook_certificate) {
          html += "              <div class='stat-row'><span class='stat-label'>Certificate:</span> <span class='stat-value'>Custom</span></div>\n";
        }
        
        if (bot.webhook_max_connections != 0) {
          html += "              <div class='stat-row'><span class='stat-label'>Max connections:</span> <span class='stat-value'>" + 
                  std::to_string(bot.webhook_max_connections) + "</span></div>\n";
        }
        
        html += "            </div>\n";
        html += "          </div>\n";
      }
      
      html += "        </div>\n";
      html += "      </div>\n";
      html += "    </div>\n";
    }
    
    html += "  </div>\n";
  }
  
  html += "<script>\n";
  html += "function copyToClipboard(text, event) {\n";
  html += "  navigator.clipboard.writeText(text)\n";
  html += "    .then(() => {\n";
  html += "      const el = event.currentTarget;\n";
  html += "      const originalText = el.textContent;\n";
  html += "      const originalBg = el.style.backgroundColor;\n";
  html += "      \n";
  html += "      el.textContent = 'Copied!';\n";
  html += "      el.style.backgroundColor = 'var(--copyable-success)';\n";
  html += "      \n";
  html += "      setTimeout(() => {\n";
  html += "        el.textContent = originalText;\n";
  html += "        el.style.backgroundColor = originalBg;\n";
  html += "      }, 1000);\n";
  html += "    })\n";
  html += "    .catch(err => {\n";
  html += "      console.error('Failed to copy: ', err);\n";
  html += "    });\n";
  html += "}\n";
  html += "\n";
  html += "function initTheme() {\n";
  html += "  const themeSwitch = document.getElementById('themeSwitch');\n";
  html += "  const themeMenu = document.getElementById('themeMenu');\n";
  html += "  const themeMenuItems = document.querySelectorAll('.theme-menu-item');\n";
  html += "  const html = document.documentElement;\n";
  html += "  \n";
  html += "  document.addEventListener('click', function(event) {\n";
  html += "    if (!themeSwitch.contains(event.target) && !themeMenu.contains(event.target)) {\n";
  html += "      themeMenu.classList.remove('visible');\n";
  html += "    }\n";
  html += "  });\n";
  html += "  \n";
  html += "  themeSwitch.addEventListener('click', function(event) {\n";
  html += "    event.stopPropagation();\n";
  html += "    themeMenu.classList.toggle('visible');\n";
  html += "  });\n";
  html += "  \n";
  html += "  function applyTheme() {\n";
  html += "    const storedTheme = localStorage.getItem('theme') || 'system';\n";
  html += "    \n";
  html += "    themeMenuItems.forEach(item => {\n";
  html += "      if (item.dataset.theme === storedTheme) {\n";
  html += "        item.classList.add('active');\n";
  html += "      } else {\n";
  html += "        item.classList.remove('active');\n";
  html += "      }\n";
  html += "    });\n";
  html += "    \n";
  html += "    if (storedTheme === 'dark') {\n";
  html += "      document.body.classList.add('dark-mode');\n";
  html += "      html.classList.remove('system-theme');\n";
  html += "    } else if (storedTheme === 'light') {\n";
  html += "      document.body.classList.remove('dark-mode');\n";
  html += "      html.classList.remove('system-theme');\n";
  html += "    } else if (storedTheme === 'system') {\n";
  html += "      html.classList.add('system-theme');\n";
  html += "      const prefersDarkMode = window.matchMedia('(prefers-color-scheme: dark)').matches;\n";
  html += "      if (prefersDarkMode) {\n";
  html += "        document.body.classList.add('dark-mode');\n";
  html += "      } else {\n";
  html += "        document.body.classList.remove('dark-mode');\n";
  html += "      }\n";
  html += "    }\n";
  html += "  }\n";
  html += "  \n";
  html += "  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function(e) {\n";
  html += "    if (localStorage.getItem('theme') === 'system') {\n";
  html += "      if (e.matches) {\n";
  html += "        document.body.classList.add('dark-mode');\n";
  html += "      } else {\n";
  html += "        document.body.classList.remove('dark-mode');\n";
  html += "      }\n";
  html += "    }\n";
  html += "  });\n";
  html += "  \n";
  html += "  themeMenuItems.forEach(item => {\n";
  html += "    item.addEventListener('click', function() {\n";
  html += "      const selectedTheme = this.dataset.theme;\n";
  html += "      localStorage.setItem('theme', selectedTheme);\n";
  html += "      themeMenu.classList.remove('visible');\n";
  html += "      applyTheme();\n";
  html += "    });\n";
  html += "  });\n";
  html += "  \n";
  html += "  applyTheme();\n";
  html += "}\n";
  html += "\n";
  html += "if (document.readyState === 'loading') {\n";
  html += "  document.addEventListener('DOMContentLoaded', initTheme);\n";
  html += "} else {\n";
  html += "  initTheme();\n";
  html += "}\n";
  html += "</script>\n";
  
  html += "</div>\n</body>\n</html>";
  
  return td::BufferSlice(html);
}

td::int64 ClientManager::get_tqueue_id(td::int64 user_id, bool is_test_dc) {
  return user_id + (static_cast<td::int64>(is_test_dc) << 54);
}

void ClientManager::start_up() {
  // init tqueue
  {
    auto load_start_time = td::Time::now();
    auto tqueue_binlog = td::make_unique<td::TQueueBinlog<td::Binlog>>();
    auto binlog = td::make_unique<td::Binlog>();
    auto tqueue = td::TQueue::create();
    td::vector<td::uint64> failed_to_replay_log_event_ids;
    td::int64 loaded_event_count = 0;
    binlog
        ->init(parameters_->working_directory_ + "tqueue.binlog",
               [&](const td::BinlogEvent &event) {
                 if (tqueue_binlog->replay(event, *tqueue).is_error()) {
                   failed_to_replay_log_event_ids.push_back(event.id_);
                 } else {
                   loaded_event_count++;
                 }
               })
        .ensure();
    tqueue_binlog.reset();

    if (!failed_to_replay_log_event_ids.empty()) {
      LOG(ERROR) << "Failed to replay " << failed_to_replay_log_event_ids.size() << " TQueue events";
      for (auto &log_event_id : failed_to_replay_log_event_ids) {
        binlog->erase(log_event_id);
      }
    }

    auto concurrent_binlog =
        std::make_shared<td::ConcurrentBinlog>(std::move(binlog), SharedData::get_binlog_scheduler_id());
    auto concurrent_tqueue_binlog = td::make_unique<td::TQueueBinlog<td::BinlogInterface>>();
    concurrent_tqueue_binlog->set_binlog(std::move(concurrent_binlog));
    tqueue->set_callback(std::move(concurrent_tqueue_binlog));

    parameters_->shared_data_->tqueue_ = std::move(tqueue);

    LOG(WARNING) << "Loaded " << loaded_event_count << " TQueue events in " << (td::Time::now() - load_start_time)
                 << " seconds";
    next_tqueue_gc_time_ = td::Time::now() + 600;
  }

  // init webhook_db
  auto concurrent_webhook_db = td::make_unique<td::BinlogKeyValue<td::ConcurrentBinlog>>();
  auto status = concurrent_webhook_db->init(parameters_->working_directory_ + "webhooks_db.binlog", td::DbKey::empty(),
                                            SharedData::get_binlog_scheduler_id());
  LOG_IF(FATAL, status.is_error()) << "Can't open webhooks_db.binlog " << status;
  parameters_->shared_data_->webhook_db_ = std::move(concurrent_webhook_db);

  auto &webhook_db = *parameters_->shared_data_->webhook_db_;
  for (const auto &key_value : webhook_db.get_all()) {
    if (!token_range_(td::to_integer<td::uint64>(key_value.first))) {
      LOG(WARNING) << "DROP WEBHOOK: " << key_value.first << " ---> " << key_value.second;
      webhook_db.erase(key_value.first);
      continue;
    }

    auto query = get_webhook_restore_query(key_value.first, key_value.second, parameters_->shared_data_);
    send_closure_later(actor_id(this), &ClientManager::send, std::move(query));
  }

  // launch watchdog
  watchdog_id_ = td::create_actor_on_scheduler<Watchdog>("ManagerWatchdog", SharedData::get_watchdog_scheduler_id(),
                                                         td::this_thread::get_id(), WATCHDOG_TIMEOUT);
  set_timeout_in(600.0);
}

PromisedQueryPtr ClientManager::get_webhook_restore_query(td::Slice token, td::Slice webhook_info,
                                                          std::shared_ptr<SharedData> shared_data) {
  // create Query with empty promise
  td::vector<td::BufferSlice> containers;
  auto add_string = [&containers](td::Slice str) {
    containers.emplace_back(str);
    return containers.back().as_mutable_slice();
  };

  token = add_string(token);

  LOG(WARNING) << "WEBHOOK: " << token << " ---> " << webhook_info;

  bool is_test_dc = false;
  if (td::ends_with(token, ":T")) {
    token.remove_suffix(2);
    is_test_dc = true;
  }

  td::ConstParser parser{webhook_info};
  td::vector<std::pair<td::MutableSlice, td::MutableSlice>> args;
  if (parser.try_skip("cert/")) {
    args.emplace_back(add_string("certificate"), add_string("previous"));
  }

  if (parser.try_skip("#maxc")) {
    args.emplace_back(add_string("max_connections"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#ip")) {
    args.emplace_back(add_string("ip_address"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#fix_ip")) {
    args.emplace_back(add_string("fix_ip_address"), add_string("1"));
    parser.skip('/');
  }

  if (parser.try_skip("#secret")) {
    args.emplace_back(add_string("secret_token"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  if (parser.try_skip("#allow")) {
    args.emplace_back(add_string("allowed_updates"), add_string(parser.read_till('/')));
    parser.skip('/');
  }

  args.emplace_back(add_string("url"), add_string(parser.read_all()));

  const auto method = add_string("setwebhook");
  auto query = td::make_unique<Query>(std::move(containers), token, is_test_dc, method, std::move(args),
                                      td::vector<std::pair<td::MutableSlice, td::MutableSlice>>(),
                                      td::vector<td::HttpFile>(), std::move(shared_data), td::IPAddress(), true);
  return PromisedQueryPtr(query.release(), PromiseDeleter(td::Promise<td::unique_ptr<Query>>()));
}

void ClientManager::dump_statistics() {
  if (is_memprof_on()) {
    LOG(WARNING) << "Memory dump:";
    td::vector<AllocInfo> v;
    dump_alloc([&](const AllocInfo &info) { v.push_back(info); });
    std::sort(v.begin(), v.end(), [](const AllocInfo &a, const AllocInfo &b) { return a.size > b.size; });
    size_t total_size = 0;
    size_t other_size = 0;
    int count = 0;
    for (auto &info : v) {
      if (count++ < 50) {
        LOG(WARNING) << td::format::as_size(info.size) << td::format::as_array(info.backtrace);
      } else {
        other_size += info.size;
      }
      total_size += info.size;
    }
    LOG(WARNING) << td::tag("other", td::format::as_size(other_size));
    LOG(WARNING) << td::tag("total size", td::format::as_size(total_size));
    LOG(WARNING) << td::tag("total traces", get_ht_size());
    LOG(WARNING) << td::tag("fast_backtrace_success_rate", get_fast_backtrace_success_rate());
  }
  auto r_mem_stat = td::mem_stat();
  if (r_mem_stat.is_ok()) {
    auto mem_stat = r_mem_stat.move_as_ok();
    LOG(WARNING) << td::tag("rss", td::format::as_size(mem_stat.resident_size_));
    LOG(WARNING) << td::tag("vm", td::format::as_size(mem_stat.virtual_size_));
    LOG(WARNING) << td::tag("rss_peak", td::format::as_size(mem_stat.resident_size_peak_));
    LOG(WARNING) << td::tag("vm_peak", td::format::as_size(mem_stat.virtual_size_peak_));
  }
  LOG(WARNING) << td::tag("buffer_mem", td::format::as_size(td::BufferAllocator::get_buffer_mem()));
  LOG(WARNING) << td::tag("buffer_slice_size", td::format::as_size(td::BufferAllocator::get_buffer_slice_size()));

  const auto &shared_data = parameters_->shared_data_;
  auto query_list_size = shared_data->query_list_size_.load(std::memory_order_relaxed);
  auto query_count = shared_data->query_count_.load(std::memory_order_relaxed);
  LOG(WARNING) << td::tag("pending queries", query_count) << td::tag("pending requests", query_list_size);

  td::uint64 i = 0;
  bool was_gap = false;
  for (auto end = &shared_data->query_list_, cur = end->prev; cur != end; cur = cur->prev, i++) {
    if (i < 20 || i > query_list_size - 20 || i % (query_list_size / 50 + 1) == 0) {
      if (was_gap) {
        LOG(WARNING) << "...";
        was_gap = false;
      }
      LOG(WARNING) << static_cast<const Query &>(*cur);
    } else {
      was_gap = true;
    }
  }

  td::dump_pending_network_queries(*parameters_->net_query_stats_);

  auto now = td::Time::now();
  auto top_clients = get_top_clients(10, {});
  for (auto top_client_id : top_clients.top_client_ids) {
    auto *client_info = clients_.get(top_client_id);
    CHECK(client_info);

    auto bot_info = client_info->client_.get_actor_unsafe()->get_bot_info();
    td::string update_count;
    td::string request_count;
    auto replace_tabs = [](td::string &str) {
      for (auto &c : str) {
        if (c == '\t') {
          c = ' ';
        }
      }
    };
    auto stats = client_info->stat_.as_vector(now);
    for (auto &stat : stats) {
      if (stat.key_ == "update_count") {
        replace_tabs(stat.value_);
        update_count = std::move(stat.value_);
      }
      if (stat.key_ == "request_count") {
        replace_tabs(stat.value_);
        request_count = std::move(stat.value_);
      }
    }
    LOG(WARNING) << td::tag("id", bot_info.id_) << td::tag("update_count", update_count)
                 << td::tag("request_count", request_count);
  }
}

void ClientManager::raw_event(const td::Event::Raw &event) {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  CHECK(info->tqueue_id_ != 0);
  auto &value = active_client_count_[info->tqueue_id_];
  if (event.ptr != nullptr) {
    value++;
  } else {
    CHECK(value > 0);
    if (--value == 0) {
      active_client_count_.erase(info->tqueue_id_);
    }
  }
}

void ClientManager::timeout_expired() {
  send_closure(watchdog_id_, &Watchdog::kick);
  set_timeout_in(WATCHDOG_TIMEOUT / 10);

  double now = td::Time::now();
  if (now > next_tqueue_gc_time_) {
    auto unix_time = parameters_->shared_data_->get_unix_time(now);
    LOG(INFO) << "Run TQueue GC at " << unix_time;
    td::int64 deleted_events;
    bool is_finished;
    std::tie(deleted_events, is_finished) = parameters_->shared_data_->tqueue_->run_gc(unix_time);
    LOG(INFO) << "TQueue GC deleted " << deleted_events << " events";
    next_tqueue_gc_time_ = td::Time::now() + (is_finished ? 60.0 : 1.0);

    tqueue_deleted_events_ += deleted_events;
    if (tqueue_deleted_events_ > last_tqueue_deleted_events_ + 10000) {
      LOG(WARNING) << "TQueue GC already deleted " << tqueue_deleted_events_ << " events since the start";
      last_tqueue_deleted_events_ = tqueue_deleted_events_;
    }
  }
}

void ClientManager::hangup_shared() {
  auto id = get_link_token();
  auto *info = clients_.get(id);
  CHECK(info != nullptr);
  info->client_.release();
  token_to_id_.erase(info->token_);
  clients_.erase(id);

  if (close_flag_ && clients_.empty()) {
    CHECK(active_client_count_.empty());
    close_db();
  }
}

void ClientManager::close_db() {
  LOG(WARNING) << "Closing databases";
  td::MultiPromiseActorSafe mpas("close binlogs");
  mpas.add_promise(td::PromiseCreator::lambda(
      [actor_id = actor_id(this)](td::Unit) { send_closure(actor_id, &ClientManager::finish_close); }));
  mpas.set_ignore_errors(true);

  auto lock = mpas.get_promise();
  parameters_->shared_data_->tqueue_->close(mpas.get_promise());
  parameters_->shared_data_->webhook_db_->close(mpas.get_promise());
  lock.set_value(td::Unit());
}

void ClientManager::finish_close() {
  LOG(WARNING) << "Stop ClientManager";
  auto promises = std::move(close_promises_);
  for (auto &promise : promises) {
    promise.set_value(td::Unit());
  }
  stop();
}

}  // namespace telegram_bot_api
