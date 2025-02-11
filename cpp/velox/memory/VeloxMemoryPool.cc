/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VeloxMemoryPool.h"
#include "compute/Backend.h"
#include "compute/VeloxBackend.h"
#include "compute/VeloxInitializer.h"
#include "velox/common/memory/MemoryAllocator.h"

using namespace facebook;

namespace gluten {

// Check if a memory pool management operation is allowed.
#define CHECK_POOL_MANAGEMENT_OP(opName)                                                                          \
  do {                                                                                                            \
    if (FOLLY_UNLIKELY(kind_ != Kind::kAggregate)) {                                                              \
      VELOX_FAIL("Memory pool {} operation is only allowed on aggregation memory pool: {}", #opName, toString()); \
    }                                                                                                             \
  } while (0)

velox::memory::MemoryManager* getDefaultVeloxMemoryManager() {
  static velox::memory::IMemoryManager::Options mmOptions{};
  static velox::memory::MemoryManager mm{mmOptions};
  return &mm;
}

// So far HbmMemoryAllocator would not work correctly since the underlying
//   gluten allocator is only used to do allocation-reporting to Spark in mmap case
class VeloxMemoryAllocator final : public velox::memory::MemoryAllocator {
 public:
  VeloxMemoryAllocator(gluten::MemoryAllocator* glutenAlloc, velox::memory::MemoryAllocator* veloxAlloc)
      : glutenAlloc_(glutenAlloc), veloxAlloc_(veloxAlloc) {}

  Kind kind() const override {
    return veloxAlloc_->kind();
  }

  bool allocateNonContiguous(
      velox::memory::MachinePageCount numPages,
      velox::memory::Allocation& out,
      ReservationCallback reservationCB,
      velox::memory::MachinePageCount minSizeClass) override {
    return veloxAlloc_->allocateNonContiguous(
        numPages,
        out,
        [this, reservationCB](int64_t allocBytes, bool preAlloc) {
          bool succeed = preAlloc ? glutenAlloc_->reserveBytes(allocBytes) : glutenAlloc_->unreserveBytes(allocBytes);
          VELOX_CHECK(succeed)
          reservationCB(allocBytes, preAlloc);
        },
        minSizeClass);
  }

  int64_t freeNonContiguous(velox::memory::Allocation& allocation) override {
    int64_t freedBytes = veloxAlloc_->freeNonContiguous(allocation);
    glutenAlloc_->unreserveBytes(freedBytes);
    return freedBytes;
  }

  bool allocateContiguous(
      velox::memory::MachinePageCount numPages,
      velox::memory::Allocation* collateral,
      velox::memory::ContiguousAllocation& allocation,
      ReservationCallback reservationCB) override {
    return veloxAlloc_->allocateContiguous(
        numPages, collateral, allocation, [this, reservationCB](int64_t allocBytes, bool preAlloc) {
          bool succeed = preAlloc ? glutenAlloc_->reserveBytes(allocBytes) : glutenAlloc_->unreserveBytes(allocBytes);
          VELOX_CHECK(succeed)
          reservationCB(allocBytes, preAlloc);
        });
  }

  void freeContiguous(velox::memory::ContiguousAllocation& allocation) override {
    const int64_t bytesToFree = allocation.size();
    glutenAlloc_->unreserveBytes(bytesToFree);
    veloxAlloc_->freeContiguous(allocation);
  }

  void* allocateBytes(uint64_t bytes, uint16_t alignment) override {
    void* out;
    VELOX_CHECK(glutenAlloc_->allocateAligned(alignment, bytes, &out))
    return out;
  }

  void freeBytes(void* p, uint64_t size) noexcept override {
    VELOX_CHECK(glutenAlloc_->free(p, size));
  }

  bool checkConsistency() const override {
    return veloxAlloc_->checkConsistency();
  }

  velox::memory::MachinePageCount numAllocated() const override {
    return veloxAlloc_->numAllocated();
  }

  velox::memory::MachinePageCount numMapped() const override {
    return veloxAlloc_->numMapped();
  }

  std::string toString() const override {
    return veloxAlloc_->toString();
  }

 private:
  gluten::MemoryAllocator* glutenAlloc_;
  velox::memory::MemoryAllocator* veloxAlloc_;
};

class VeloxMemoryPool : public velox::memory::MemoryPoolImpl {
 public:
  VeloxMemoryPool(
      std::shared_ptr<MemoryPool> parent,
      const std::string& name,
      Kind kind,
      velox::memory::MemoryManager* manager,
      std::unique_ptr<velox::memory::MemoryReclaimer> reclaimer = nullptr,
      DestructionCallback destructionCb = nullptr,
      const Options& options = Options{})
      : velox::memory::MemoryPoolImpl(manager, name, kind, parent, std::move(reclaimer), destructionCb, options) {}

  void setAllocatorShared(std::shared_ptr<velox::memory::MemoryAllocator> sharedAlloc) {
    setAllocator(sharedAlloc.get());
    sharedAlloc_ = sharedAlloc;
  }

 protected:
  std::shared_ptr<MemoryPool> genChild(
      std::shared_ptr<MemoryPool> parent,
      const std::string& name,
      Kind kind,
      bool threadSafe,
      std::unique_ptr<velox::memory::MemoryReclaimer> reclaimer) override {
    const std::shared_ptr<VeloxMemoryPool>& child = std::make_shared<VeloxMemoryPool>(
        parent,
        name,
        kind,
        getDefaultVeloxMemoryManager(),
        std::move(reclaimer),
        nullptr,
        Options{
            .alignment = alignment_,
            .trackUsage = trackUsage_,
            .threadSafe = threadSafe,
            .checkUsageLeak = checkUsageLeak_});
    if (sharedAlloc_) {
      child->setAllocatorShared(sharedAlloc_);
    }
    return child;
  }

 private:
  std::shared_ptr<velox::memory::MemoryAllocator> sharedAlloc_; // workaround to keep the allocator instance alive
};

static std::shared_ptr<velox::memory::MemoryPool> rootVeloxMemoryPool() {
  static auto options = gluten::VeloxInitializer::get()->getMemoryPoolOptions();
  int64_t spillThreshold = gluten::VeloxInitializer::get()->getSpillThreshold();
  static std::shared_ptr<VeloxMemoryPool> defaultPoolRoot = std::make_shared<VeloxMemoryPool>(
      nullptr,
      "root",
      velox::memory::MemoryPool::Kind::kAggregate,
      getDefaultVeloxMemoryManager(),
      facebook::velox::memory::MemoryReclaimer::create(),
      nullptr,
      options);
  defaultPoolRoot->setHighUsageCallback(
      [=](velox::memory::MemoryPool& pool) { return pool.reservedBytes() >= spillThreshold; });
  return defaultPoolRoot;
}

std::shared_ptr<velox::memory::MemoryPool> defaultLeafVeloxMemoryPool() {
  static std::shared_ptr<velox::memory::MemoryPool> defaultPool = rootVeloxMemoryPool()->addLeafChild("default_leaf");
  return defaultPool;
}

std::shared_ptr<velox::memory::MemoryPool> asAggregateVeloxMemoryPool(gluten::MemoryAllocator* allocator) {
  static std::atomic_uint32_t id = 0;
  auto pool = rootVeloxMemoryPool()->addAggregateChild(
      "wrapped_root" + std::to_string(id++), facebook::velox::memory::MemoryReclaimer::create());
  auto wrapped = std::dynamic_pointer_cast<VeloxMemoryPool>(pool);
  VELOX_CHECK_NOT_NULL(wrapped);
  velox::memory::MemoryAllocator* veloxAlloc = wrapped->getAllocator();
  wrapped->setAllocatorShared(std::make_shared<VeloxMemoryAllocator>(allocator, veloxAlloc));
  return pool;
}

} // namespace gluten
