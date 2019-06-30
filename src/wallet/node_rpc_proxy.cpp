// Copyright (c) 2018-2019, The Arqma Network
// Copyright (c) 2017-2018, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "node_rpc_proxy.h"
#include "wallet_rpc_helpers.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "rpc/rpc_payment_signature.h"
#include "rpc/rpc_payment_costs.h"
#include "storages/http_abstract_invoke.h"

#define RETURN_ON_RPC_RESPONSE_ERROR(r, error, res, method) \
  do { \
    CHECK_AND_ASSERT_MES(error.code == 0, error.message, error.message); \
    handle_payment_changes(res, std::integral_constant<bool, HasCredits<decltype(res)>::Has>()); \
    CHECK_AND_ASSERT_MES(r, std::string(), "Failed to connect to daemon"); \
    /* empty string -> not connection */ \
    CHECK_AND_ASSERT_MES(!res.status.empty(), res.status, "No connection to daemon"); \
    CHECK_AND_ASSERT_MES(res.status != CORE_RPC_STATUS_BUSY, res.status, "Daemon busy"); \
    CHECK_AND_ASSERT_MES(res.status != CORE_RPC_STATUS_PAYMENT_REQUIRED, res.status, "Payment required"); \
    CHECK_AND_ASSERT_MES(res.status == CORE_RPC_STATUS_OK, res.status, "Error calling " + std::string(method) + " daemon RPC"); \
  } while(0)

using namespace epee;

namespace tools
{

static const std::chrono::seconds rpc_timeout = std::chrono::minutes(3) + std::chrono::seconds(30);

NodeRPCProxy::NodeRPCProxy(epee::net_utils::http::http_simple_client &http_client, rpc_payment_state_t &rpc_payment_state, boost::recursive_mutex &mutex)
  : m_http_client(http_client)
  , m_rpc_payment_state(rpc_payment_state)
  , m_daemon_rpc_mutex(mutex)
  , m_offline(false)
{
  invalidate();
}

void NodeRPCProxy::invalidate()
{
  m_height = 0;
  for (size_t n = 0; n < 256; ++n)
    m_earliest_height[n] = 0;
  m_dynamic_per_kb_fee_estimate = 0;
  m_dynamic_per_kb_fee_estimate_cached_height = 0;
  m_dynamic_per_kb_fee_estimate_grace_blocks = 0;
  m_rpc_version = 0;
  m_target_height = 0;
  m_block_size_limit = 0;
  m_get_info_time = 0;
  m_rpc_payment_info_time = 0;
}

boost::optional<std::string> NodeRPCProxy::get_rpc_version(uint32_t &rpc_version)
{
  if(m_offline)
    return boost::optional<std::string>("offline");
  if(m_rpc_version == 0)
  {
    cryptonote::COMMAND_RPC_GET_VERSION::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_GET_VERSION::response resp_t = AUTO_VAL_INIT(resp_t);
    {
      const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_version", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_version");
    }
    m_rpc_version = resp_t.version;
  }
  rpc_version = m_rpc_version;
  return boost::optional<std::string>();
}

void NodeRPCProxy::set_height(uint64_t h)
{
  m_height = h;
}

boost::optional<std::string> NodeRPCProxy::get_info()
{
  if(m_offline)
    return boost::optional<std::string>("offline");
  const time_t now = time(NULL);
  if (now >= m_get_info_time + 30) // re-cache every 30 seconds
  {
    cryptonote::COMMAND_RPC_GET_INFO::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_GET_INFO::response resp_t = AUTO_VAL_INIT(resp_t);

    {
      const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
      uint64_t pre_call_credits = m_rpc_payment_state.credits;
      req_t.client = cryptonote::make_rpc_payment_signature(m_client_id_secret_key);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_info", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_info");
      check_rpc_cost(m_rpc_payment_state, "get_info", resp_t.credits, pre_call_credits, COST_PER_GET_INFO);
    }

    m_height = resp_t.height;
    m_target_height = resp_t.target_height;
    m_block_size_limit = resp_t.block_size_limit;
    m_get_info_time = now;
  }
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_height(uint64_t &height)
{
  auto res = get_info();
  if (res)
    return res;
  height = m_height;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_target_height(uint64_t &height)
{
  auto res = get_info();
  if (res)
    return res;
  height = m_target_height;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_block_size_limit(uint64_t &block_size_limit)
{
  auto res = get_info();
  if (res)
    return res;
  block_size_limit = m_block_size_limit;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_earliest_height(uint8_t version, uint64_t &earliest_height)
{
  if(m_offline)
    return boost::optional<std::string>("offline");
  if (m_earliest_height[version] == 0)
  {
    cryptonote::COMMAND_RPC_HARD_FORK_INFO::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_HARD_FORK_INFO::response resp_t = AUTO_VAL_INIT(resp_t);

    req_t.version = version;

    {
      const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
      uint64_t pre_call_credits = m_rpc_payment_state.credits;
      req_t.client = cryptonote::make_rpc_payment_signature(m_client_id_secret_key);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "hard_fork_info", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "hard_fork_info");
      check_rpc_cost(m_rpc_payment_state, "hard_fork_info", resp_t.credits, pre_call_credits, COST_PER_HARD_FORK_INFO);
    }

    m_earliest_height[version] = resp_t.earliest_height;
  }

  earliest_height = m_earliest_height[version];
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_dynamic_per_kb_fee_estimate(uint64_t grace_blocks, uint64_t &fee)
{
  uint64_t height;

  boost::optional<std::string> result = get_height(height);
  if (result)
    return result;

  if(m_offline)
    return boost::optional<std::string>("offline");
  if (m_dynamic_per_kb_fee_estimate_cached_height != height || m_dynamic_per_kb_fee_estimate_grace_blocks != grace_blocks)
  {
    cryptonote::COMMAND_RPC_GET_PER_KB_FEE_ESTIMATE::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_GET_PER_KB_FEE_ESTIMATE::response resp_t = AUTO_VAL_INIT(resp_t);

    {
      const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
      uint64_t pre_call_credits = m_rpc_payment_state.credits;
      req_t.client = cryptonote::make_rpc_payment_signature(m_client_id_secret_key);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "get_fee_estimate", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "get_fee_estimate");
      check_rpc_cost(m_rpc_payment_state, "get_fee_estimate", resp_t.credits, pre_call_credits, COST_PER_FEE_ESTIMATE);
    }

    m_dynamic_per_kb_fee_estimate = resp_t.fee;
    m_dynamic_per_kb_fee_estimate_cached_height = height;
    m_dynamic_per_kb_fee_estimate_grace_blocks = grace_blocks;
  }

  fee = m_dynamic_per_kb_fee_estimate;
  return boost::optional<std::string>();
}

boost::optional<std::string> NodeRPCProxy::get_rpc_payment_info(bool mining, bool &payments, uint64_t &credits, uint64_t &diff, uint64_t &credits_per_hash_found, cryptonote::blobdata &blob, uint64_t &height)
{
  const time_t now = time(NULL);
  if (m_rpc_payment_state.stale || now >= m_rpc_payment_info_time + 5*60 || (mining && now >= m_rpc_payment_info_time + 10)) // re-cache every 10 seconds if mining, 5 minutes otherwise
  {
    cryptonote::COMMAND_RPC_ACCESS_INFO::request req_t = AUTO_VAL_INIT(req_t);
    cryptonote::COMMAND_RPC_ACCESS_INFO::response resp_t = AUTO_VAL_INIT(resp_t);

    {
      const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
      req_t.client = cryptonote::make_rpc_payment_signature(m_client_id_secret_key);
      bool r = net_utils::invoke_http_json_rpc("/json_rpc", "rpc_access_info", req_t, resp_t, m_http_client, rpc_timeout);
      RETURN_ON_RPC_RESPONSE_ERROR(r, epee::json_rpc::error{}, resp_t, "rpc_access_info");
      m_rpc_payment_state.stale = false;
    }

    m_rpc_payment_payments = resp_t.diff > 0;
    m_rpc_payment_credits = resp_t.credits;
    m_rpc_payment_diff = resp_t.diff;
    m_rpc_payment_credits_per_hash_found = resp_t.credits_per_hash_found;
    m_rpc_payment_blob = resp_t.hashing_blob;
    m_rpc_payment_height = resp_t.height;

    if (!epee::string_tools::parse_hexstr_to_binbuff(resp_t.hashing_blob, m_rpc_payment_blob) || m_rpc_payment_blob.size() < 43)
    {
      MERROR("Invalid hashing blob: " << resp_t.hashing_blob);
      return boost::optional<std::string>("Invalid hashing blob");
    }
    m_rpc_payment_info_time = now;
  }

  payments = m_rpc_payment_payments;
  credits = m_rpc_payment_credits;
  diff = m_rpc_payment_diff;
  credits_per_hash_found = m_rpc_payment_credits_per_hash_found;
  blob = m_rpc_payment_blob;
  height = m_rpc_payment_height;
  return boost::optional<std::string>();
}

}
