// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <vector>

#include <boost/scope_exit.hpp>

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "yb/server/logical_clock.h"
#include "yb/tablet/mvcc.h"
#include "yb/util/random_util.h"
#include "yb/util/test_util.h"
#include "yb/util/enums.h"

using namespace std::literals;
using std::vector;

using yb::server::LogicalClock;

namespace yb {
namespace tablet {

class MvccTest : public YBTest {
 public:
  MvccTest()
      : clock_(server::LogicalClock::CreateStartingAt(HybridTime::kInitial)),
        manager_(std::string(), clock_.get()) {
  }

 protected:
  void RunRandomizedTest(bool use_ht_lease);

  server::ClockPtr clock_;
  MvccManager manager_;
};

namespace {

HybridTime AddLogical(HybridTime input, uint64_t delta) {
  HybridTime result;
  EXPECT_OK(result.FromUint64(input.ToUint64() + delta));
  return result;
}

}

TEST_F(MvccTest, Basic) {
  constexpr size_t kTotalEntries = 10;
  vector<HybridTime> hts(kTotalEntries);
  for (auto& ht : hts) {
    manager_.AddPending(&ht);
  }
  for (const auto& ht : hts) {
    manager_.Replicated(ht);
    ASSERT_EQ(ht, manager_.LastReplicatedHybridTime());
  }
}

TEST_F(MvccTest, SafeHybridTimeToReadAt) {
  constexpr uint64_t kLease = 10;
  constexpr uint64_t kDelta = 10;
  auto ht_lease = AddLogical(clock_->Now(), kLease);
  clock_->Update(AddLogical(ht_lease, kDelta));
  ASSERT_EQ(ht_lease, manager_.SafeTime(ht_lease));

  HybridTime ht1 = clock_->Now();
  manager_.AddPending(&ht1);
  ASSERT_EQ(ht1.Decremented(), manager_.SafeTime());

  HybridTime ht2;
  manager_.AddPending(&ht2);
  ASSERT_EQ(ht1.Decremented(), manager_.SafeTime());

  manager_.Replicated(ht1);
  ASSERT_EQ(ht2.Decremented(), manager_.SafeTime());

  manager_.Replicated(ht2);
  auto now = clock_->Now();

  ASSERT_EQ(now, manager_.SafeTime(now));
}

TEST_F(MvccTest, Abort) {
  constexpr size_t kTotalEntries = 10;
  vector<HybridTime> hts(kTotalEntries);
  for (auto& ht : hts) {
    manager_.AddPending(&ht);
  }
  for (size_t i = 1; i < hts.size(); i += 2) {
    manager_.Aborted(hts[i]);
  }
  for (size_t i = 0; i < hts.size(); i += 2) {
    ASSERT_EQ(hts[i].Decremented(), manager_.SafeTime());
    manager_.Replicated(hts[i]);
  }
  auto now = clock_->Now();
  ASSERT_EQ(now, manager_.SafeTime(now));
}

void MvccTest::RunRandomizedTest(bool use_ht_lease) {
  constexpr size_t kTotalOperations = 20000;
  enum class Op { kAdd, kReplicated, kAborted };

  std::map<HybridTime, size_t> queue;
  vector<HybridTime> alive;
  size_t counts[] = { 0, 0, 0 };

  std::atomic<bool> stopped { false };

  const auto get_count = [&counts](Op op) { return counts[to_underlying(op)]; };
  LogicalClock* const logical_clock = down_cast<LogicalClock*>(clock_.get());

  std::atomic<uint64_t> max_ht_lease{0};
  std::atomic<bool> is_leader{true};

  const auto ht_lease_provider =
      [use_ht_lease, logical_clock, &max_ht_lease]() -> HybridTime {
        if (!use_ht_lease)
          return HybridTime::kMax;
        auto ht_lease = logical_clock->Peek().AddMicroseconds(RandomUniformInt(0, 50));

        // Update the maximum HT lease that we gave to any caller.
        UpdateAtomicMax(&max_ht_lease, ht_lease.ToUint64());

        return ht_lease;
      };

  // This thread will keep getting the safe time in the background.
  std::thread safetime_query_thread([this, &stopped, &ht_lease_provider, &is_leader]() {
    while (!stopped.load(std::memory_order_acquire)) {
      if (is_leader.load(std::memory_order_acquire)) {
        manager_.SafeTime(HybridTime::kMin, MonoTime::kMax, ht_lease_provider());
      } else {
        manager_.SafeTimeForFollower(HybridTime::kMin, MonoTime::kMax);
      }
      std::this_thread::yield();
    }
  });

  BOOST_SCOPE_EXIT(&stopped, &safetime_query_thread) {
    stopped = true;
    safetime_query_thread.join();
  } BOOST_SCOPE_EXIT_END;

  vector<std::pair<Op, HybridTime>> ops;
  ops.reserve(kTotalOperations);

  const int kTargetConcurrency = 50;

  for (size_t i = 0; i < kTotalOperations || !alive.empty(); ++i) {
    int rnd;
    if (kTotalOperations - i <= alive.size()) {
      // We have (kTotalOperations - i) operations left to do, so let's finish operations that are
      // already in progress.
      rnd = kTargetConcurrency + RandomUniformInt(0, 1);
    } else {
      // If alive.size() < kTargetConcurrency, we'll be starting new operations with probability of
      // 1 - alive.size() / (2 * kTargetConcurrency), starting at almost 100% and approaching 50%
      // as alive.size() reaches kTargetConcurrency.
      //
      // If alive.size() >= kTargetConcurrency: we keep starting new operations in half of the
      // cases, and finishing existing ones in half the cases.
      rnd = RandomUniformInt(-kTargetConcurrency, kTargetConcurrency - 1) +
          std::min<int>(kTargetConcurrency, alive.size());
    }
    if (rnd < kTargetConcurrency) {
      // Start a new operation.
      HybridTime ht;
      manager_.AddPending(&ht);
      alive.push_back(ht);
      queue.emplace(alive.back(), alive.size() - 1);
      ops.emplace_back(Op::kAdd, alive.back());
    } else {
      size_t idx;
      if (rnd & 1) {
        // Finish replication for the next operation.
        idx = queue.begin()->second;
        ops.emplace_back(Op::kReplicated, alive[idx]);
        manager_.Replicated(alive[idx]);
      } else {
        // Abort a random operation that is alive.
        idx = RandomUniformInt<size_t>(0, alive.size() - 1);
        ops.emplace_back(Op::kAborted, alive[idx]);
        manager_.Aborted(alive[idx]);
      }
      queue.erase(alive[idx]);
      alive[idx] = alive.back();
      alive.pop_back();
      if (idx != alive.size()) {
        ASSERT_EQ(queue[alive[idx]], alive.size());
        queue[alive[idx]] = idx;
      }
    }
    ++counts[to_underlying(ops.back().first)];

    HybridTime safe_time;
    if (alive.empty()) {
      auto time_before = clock_->Now();
      safe_time = manager_.SafeTime(ht_lease_provider());
      auto time_after = clock_->Now();
      ASSERT_GE(safe_time.ToUint64(), time_before.ToUint64());
      ASSERT_LE(safe_time.ToUint64(), time_after.ToUint64());
    } else {
      auto min = queue.begin()->first;
      safe_time = manager_.SafeTime(ht_lease_provider());
      ASSERT_EQ(min.Decremented(), safe_time);
    }
    if (use_ht_lease) {
      ASSERT_LE(safe_time.ToUint64(), max_ht_lease.load(std::memory_order_acquire));
    }
  }
  LOG(INFO) << "Adds: " << get_count(Op::kAdd)
            << ", replicates: " << get_count(Op::kReplicated)
            << ", aborts: " << get_count(Op::kAborted);
  const size_t replicated_and_aborted = get_count(Op::kReplicated) + get_count(Op::kAborted);
  ASSERT_EQ(kTotalOperations, get_count(Op::kAdd) + replicated_and_aborted);
  ASSERT_EQ(get_count(Op::kAdd), replicated_and_aborted);

  // Replay the recorded operations as if we are a follower receiving these operations from the
  // leader.
  is_leader = false;
  uint64_t shift = std::max(max_ht_lease + 1, clock_->Now().ToUint64() + 1);
  LOG(INFO) << "Shifting hybrid times by " << shift << " units and replaying in follower mode";
  auto start = std::chrono::steady_clock::now();
  for (auto& op : ops) {
    op.second = HybridTime(op.second.ToUint64() + shift);
    switch (op.first) {
      case Op::kAdd:
        manager_.AddPending(&op.second);
        break;
      case Op::kReplicated:
        manager_.Replicated(op.second);
        break;
      case Op::kAborted:
        manager_.Aborted(op.second);
        break;
    }
  }
  auto end = std::chrono::steady_clock::now();
  LOG(INFO) << "Passed: " << yb::ToString(end - start);
}

TEST_F(MvccTest, RandomWithoutHTLease) {
  RunRandomizedTest(false);
}

TEST_F(MvccTest, RandomWithHTLease) {
  RunRandomizedTest(true);
}

TEST_F(MvccTest, WaitForSafeTime) {
  constexpr uint64_t kLease = 10;
  constexpr uint64_t kDelta = 10;
  auto limit = AddLogical(clock_->Now(), kLease);
  clock_->Update(AddLogical(limit, kDelta));
  HybridTime ht1 = clock_->Now();
  manager_.AddPending(&ht1);
  HybridTime ht2;
  manager_.AddPending(&ht2);
  std::atomic<bool> t1_done(false);
  std::thread t1([this, ht2, &t1_done] {
    manager_.SafeTime(ht2.Decremented(), MonoTime::kMax, HybridTime::kMax);
    t1_done = true;
  });
  std::atomic<bool> t2_done(false);
  std::thread t2([this, ht2, &t2_done] {
    manager_.SafeTime(AddLogical(ht2, 1), MonoTime::kMax, HybridTime::kMax);
    t2_done = true;
  });
  std::this_thread::sleep_for(100ms);
  ASSERT_FALSE(t1_done.load());
  ASSERT_FALSE(t2_done.load());

  manager_.Replicated(ht1);
  std::this_thread::sleep_for(100ms);
  ASSERT_TRUE(t1_done.load());
  ASSERT_FALSE(t2_done.load());

  manager_.Replicated(ht2);
  std::this_thread::sleep_for(100ms);
  ASSERT_TRUE(t1_done.load());
  ASSERT_TRUE(t2_done.load());

  t1.join();
  t2.join();

  HybridTime ht3;
  manager_.AddPending(&ht3);
  ASSERT_FALSE(manager_.SafeTime(ht3, MonoTime::Now() + 100ms, HybridTime::kMax));
}

} // namespace tablet
} // namespace yb
