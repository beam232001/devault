// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/consensus.h>
#include <rpc/server.h>
#include <test/test_bitcoin.h>

#include "catch_unit.h"
#include "callrpc.h"
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <limits>
#include <string>

// BOOST_FIXTURE_TEST_SUITE(excessiveblock_tests, TestingSetup)

TEST_CASE("excessiveblock_rpc") {
  TestingSetup setup;
  BOOST_CHECK_NO_THROW(CallRPC("getexcessiveblock"));

  BOOST_CHECK_THROW(CallRPC("setexcessiveblock"), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC("setexcessiveblock not_uint"), std::invalid_argument);
  BOOST_CHECK_THROW(CallRPC("setexcessiveblock 1000000 not_uint"), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC("setexcessiveblock 1000000 1"), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC("setexcessiveblock -1"), std::runtime_error);

  BOOST_CHECK_THROW(CallRPC("setexcessiveblock 0"), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC("setexcessiveblock 1"), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC("setexcessiveblock 1000"), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC(std::string("setexcessiveblock ") + std::to_string(ONE_MEGABYTE - 1)), std::runtime_error);
  BOOST_CHECK_THROW(CallRPC(std::string("setexcessiveblock ") + std::to_string(ONE_MEGABYTE)), std::runtime_error);

  BOOST_CHECK_NO_THROW(CallRPC(std::string("setexcessiveblock ") + std::to_string(ONE_MEGABYTE + 1)));
  BOOST_CHECK_NO_THROW(CallRPC(std::string("setexcessiveblock ") + std::to_string(ONE_MEGABYTE + 10)));

  // Default can be higher than 1MB in future - test it too
  BOOST_CHECK_NO_THROW(CallRPC(std::string("setexcessiveblock ") + std::to_string(DEFAULT_MAX_BLOCK_SIZE)));
  BOOST_CHECK_NO_THROW(CallRPC(std::string("setexcessiveblock ") + std::to_string(DEFAULT_MAX_BLOCK_SIZE * 8)));

  BOOST_CHECK_NO_THROW(
      CallRPC(std::string("setexcessiveblock ") + std::to_string(std::numeric_limits<int64_t>::max())));

  BOOST_CHECK_THROW(
      CallRPC(std::string("setexcessiveblock ") + std::to_string(std::numeric_limits<uint64_t>::max() + 1)),
      std::runtime_error);
}

// BOOST_AUTO_TEST_SUITE_END()
