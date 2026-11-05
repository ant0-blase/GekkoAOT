// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
namespace GekkoAOT::GX::HD {
// Worker-only accounting. Generation must not recursively stat the complete
// library after every texture, nor race other workers evicting the same files.
class DiskBudget {
  std::mutex mutex_;
  std::once_flag start_;
  std::filesystem::path root_;
  std::unordered_map<std::filesystem::path,std::uint64_t> sizes_;
  std::uint64_t total_=0,scans_=0;
  std::chrono::steady_clock::time_point scanned_{};
  struct File {std::filesystem::path path;std::uint64_t size;std::filesystem::file_time_type time;};
  std::vector<File> Scan(const std::filesystem::path& root){
    ++scans_;sizes_.clear();total_=0;std::vector<File> files;std::error_code ec;
    for(std::filesystem::recursive_directory_iterator it(root,ec),end;!ec && it!=end;it.increment(ec)){
      if(it->path().extension()!=".dds")continue;
      const auto size=it->file_size(ec);if(ec){ec.clear();continue;}
      const auto time=it->last_write_time(ec);if(ec){ec.clear();continue;}
      files.push_back({it->path(),size,time});sizes_[it->path()]=size;total_+=size;
    }
    root_=root;scanned_=std::chrono::steady_clock::now();return files;
  }
  std::jthread scanner_; // destroyed/joined before the state it accesses
public:
  void Start(const std::filesystem::path& root){
    std::call_once(start_,[&]{scanner_=std::jthread([this,root]{std::lock_guard lock(mutex_);Scan(root);});});
  }
  bool Contains(const std::filesystem::path& path){
    // Startup catalogue work must never block a draw. The observation path
    // retries next frame if the initial scan is still in progress.
    std::unique_lock lock(mutex_,std::try_to_lock);return lock.owns_lock() && sizes_.contains(path);
  }
  void Written(const std::filesystem::path& root,const std::filesystem::path& path,std::uint64_t size,std::uint64_t limit){
    std::lock_guard lock(mutex_);std::vector<File> files;
    const bool refresh=root_!=root || std::chrono::steady_clock::now()-scanned_>std::chrono::minutes(5);
    if(refresh)files=Scan(root);
    const auto old=sizes_.find(path);if(old!=sizes_.end())total_-=old->second;
    sizes_[path]=size;total_+=size;
    if(total_<=limit)return;
    if(!refresh)files=Scan(root);
    // Evict to a small reserve rather than rescan on every subsequent write.
    const auto target=limit-limit/10;
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.time<b.time;});
    for(const auto& file:files){
      if(total_<=target)break;
      // A worker can have committed a more recent version after the scan.
      // Keep the resource just committed by this caller whenever it fits.
      if(file.path==path && file.size<=target)continue;
      std::error_code ec;if(std::filesystem::remove(file.path,ec)){total_-=file.size;sizes_.erase(file.path);}
    }
  }
  std::uint64_t scans(){std::lock_guard lock(mutex_);return scans_;}
  std::uint64_t bytes(){std::lock_guard lock(mutex_);return total_;}
};
}
