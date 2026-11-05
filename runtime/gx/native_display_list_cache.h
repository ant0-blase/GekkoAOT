// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_gx_ir.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>

namespace GekkoAOT::GX {
// Bounded LRU index of translated lists, keyed by guest address. It replaces
// an eight-slot linear scan that thrashed when a stage calls hundreds of lists
// per frame (Super Monkey Ball: ~96% misses, each one re-copying and
// re-translating its list before executing it). The index only locates a
// candidate: every hit still compares the exact source bytes (and the caller's
// entry state), so a guest write without host notification is never missed.
template <class Entry> class NativeListLru {
public:
  static constexpr std::size_t DefaultEntries = 2048, MaxCost = std::size_t(32) << 20;
  // A/B and bisection only: GEKKOAOT_NATIVE_GX_LIST_CACHE=8 restores the
  // former capacity. Shrinking evicts immediately.
  void SetCapacity(std::size_t entries) {
    max_entries_ = std::max<std::size_t>(entries, 1);
    while (index_.size() > max_entries_) {
      cost_ -= order_.back().cost; index_.erase(order_.back().address); order_.pop_back();
    }
  }
  Entry* Find(std::uint32_t address) {
    const auto it = index_.find(address);
    if (it == index_.end()) return nullptr;
    order_.splice(order_.begin(), order_, it->second);
    return &*it->second;
  }
  // The entry for this address (created when absent) becomes most recent.
  // The caller refills it, then commits the memory it retains.
  Entry& Acquire(std::uint32_t address) {
    if (auto* entry = Find(address)) { cost_ -= entry->cost; entry->cost = 0; return *entry; }
    order_.emplace_front();
    order_.front().address = address;
    index_.emplace(address, order_.begin());
    return order_.front();
  }
  void Commit(Entry& entry, std::size_t cost) {
    entry.cost = cost; cost_ += cost;
    while ((index_.size() > max_entries_ || cost_ > MaxCost) && &order_.back() != &entry) {
      cost_ -= order_.back().cost;
      index_.erase(order_.back().address);
      order_.pop_back();
    }
  }
  void Reset() { order_.clear(); index_.clear(); cost_ = 0; }
  std::size_t Size() const noexcept { return index_.size(); }
  std::size_t Cost() const noexcept { return cost_; }
private:
  std::list<Entry> order_;
  std::unordered_map<std::uint32_t, typename std::list<Entry>::iterator> index_;
  std::size_t cost_ = 0, max_entries_ = DefaultEntries;
};

// Engines that rebuild their display lists every frame (EA's GameCube
// renderers emit PS2-style DMA packets: Medal of Honor) find a known address
// with new bytes on nearly every call, and each such miss copied the list,
// translated it and refilled its entry before running exactly the fallback
// path a declined lookup takes. When at least 240 of the last 256 lookups
// were such content changes, lookups are declined without that work for the
// next 8192, then the cache is probed again. First-time addresses (a new
// scene) and hits never count toward the threshold.
class NativeListMissGovernor {
public:
  static constexpr unsigned Window = 256, MissThreshold = 240, BypassLookups = 8192;
  bool Bypass() noexcept {
    if (bypass_left_ == 0) return false;
    --bypass_left_; ++bypassed;
    return true;
  }
  void Note(bool changed) noexcept {
    misses_ += changed ? 1u : 0u;
    if (++window_ < Window) return;
    if (misses_ >= MissThreshold) { bypass_left_ = BypassLookups; ++episodes; }
    window_ = misses_ = 0;
  }
  void Reset() noexcept { window_ = misses_ = bypass_left_ = 0; bypassed = episodes = 0; }
  std::uint64_t bypassed = 0, episodes = 0;
private:
  unsigned window_ = 0, misses_ = 0, bypass_left_ = 0;
};

// State-only lists have no VAT-dependent framing or borrowed vertex pointers.
// Cache their translation, not the resulting state: masks, copies and PE events
// must execute on every call against the current state. Exact byte comparison
// also observes guest writes that have no host invalidation notification.
class NativeDisplayListCache {
public:
  static constexpr unsigned MaxBytes = 65536, MaxOperations = 4096;
  struct Stats { std::uint64_t hits=0, misses=0, changed=0, negative_hits=0, declined=0, operations=0; } stats;
  struct Plan {
    const std::vector<NativeOperation>* operations=nullptr;
    std::uint32_t commands=0;
    bool cached=false;
    // All admitted sizes retain the exact bytes used for recognition, even
    // when primitives need state-dependent decoding. Valid until the next
    // Find/Reset; callers must consume synchronously and cannot retain it.
    std::span<const std::uint8_t> source{};
  };
  static bool Translate(std::span<const std::uint8_t> source, std::vector<NativeOperation>& output,
                        std::uint32_t& commands) {
    output.clear();commands=0;
    if(source.empty() || source.size()>MaxBytes)return false;
    return Compile(source,output,commands);
  }
  void Reset() { lru_.Reset(); stats={}; governor.Reset(); }
  void SetCapacity(std::size_t entries) { lru_.SetCapacity(entries); }
  std::size_t Entries() const noexcept { return lru_.Size(); }
  NativeListMissGovernor governor;
  Plan Find(std::uint32_t address, std::span<const std::uint8_t> source) {
    if (source.empty() || source.size()>MaxBytes) { ++stats.declined;return {}; }
    if (governor.Bypass()) return {};
    if(auto* e=lru_.Find(address)) {
      if(e->bytes.size()==source.size() && std::memcmp(e->bytes.data(),source.data(),source.size())==0) {
        governor.Note(false);
        if(!e->compiled){++stats.negative_hits;return {nullptr,0,true,e->bytes};}
        ++stats.hits;return {&e->operations,e->commands,true,e->bytes};
      }
      ++stats.changed;governor.Note(true);
    } else governor.Note(false);
    ++stats.misses;auto& slot=lru_.Acquire(address);
    slot.bytes.assign(source.begin(),source.end());slot.operations.clear();slot.commands=0;
    slot.compiled=Translate(slot.bytes,slot.operations,slot.commands);
    if(!slot.compiled){slot.operations.clear();++stats.declined;}
    else stats.operations+=slot.operations.size();
    lru_.Commit(slot,sizeof(Entry)+slot.bytes.size()+slot.operations.size()*sizeof(NativeOperation));
    if(!slot.compiled)return {nullptr,0,false,slot.bytes};
    return {&slot.operations,slot.commands,false,slot.bytes};
  }
private:
  struct Entry {
    std::vector<std::uint8_t> bytes;
    std::vector<NativeOperation> operations;
    std::uint32_t address=0,commands=0;
    std::size_t cost=0;
    bool compiled=false;
  };
  NativeListLru<Entry> lru_;
  static std::uint32_t Word(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
  }
  static bool Compile(std::span<const std::uint8_t> bytes, std::vector<NativeOperation>& output,
                      std::uint32_t& commands) {
    for(std::size_t offset=0;offset<bytes.size();) {
      const auto* p=bytes.data()+offset;const auto remaining=bytes.size()-offset;
      const auto op=p[0]&0xf8u;std::size_t length=1;
      NativeOperation operation;
      if(op==0) { ++offset;++commands;continue; }
      if(op==8 && remaining>=6) {
        length=6;if(!LowerNativeCp(p[1],Word(p+2),operation))return false;
      } else if(op==0x10 && remaining>=5) {
        const auto header=Word(p+1),words=(header>>16)+1u;length=5u+4u*words;
        if(length>remaining || !LowerNativeXf(header&0xffffu,words,{p+5,length-5},operation))return false;
      } else if(op==0x60 && remaining>=5) {
        NativePixelState pixel;const auto word=Word(p+1);length=5;
        if(!LowerNativeBp(word>>24,word,pixel))return false;operation=pixel;
      } else return false;
      if(output.size()==MaxOperations || !ValidNativeOperation(operation))return false;
      output.push_back(operation);offset+=length;++commands;
    }
    return true;
  }
};
} // namespace GekkoAOT::GX
