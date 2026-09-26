#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace GekkoAOT::HLE {
enum class Execution { Native, GuestBridge, Compatibility, Unsupported };
enum class Behaviour { Synchronous, Asynchronous };
enum class ReturnABI { Void, R3, R3R4 };
inline const char* Name(Execution value) {
  switch (value) {
  case Execution::Native: return "native";
  case Execution::GuestBridge: return "guest-bridge";
  case Execution::Compatibility: return "compatibility";
  case Execution::Unsupported: return "unsupported";
  }
  return "unsupported";
}
struct ABI {
  // Integer arguments occupy r3..r10. Guest pointers are always 32-bit values.
  std::uint8_t integer_arguments;
  ReturnABI result;
  bool guest_pointers;
};
template<class Context, class Id> struct Descriptor {
  Id id;
  const char* name;
  ABI abi;
  Behaviour behaviour;
  Execution execution;
  bool (*invoke)(Context*);
};
// Descriptors own dispatch policy; the existing address recognizer owns proof,
// code guards and transient return interceptions. No unchecked address admission.
template<class Context, class Id> class Registry {
public:
  using Item = Descriptor<Context, Id>;
  bool Add(Item item) {
    if (!item.name || !*item.name || by_name_.count(item.name) || items_.count(item.id)) return false;
    if (item.execution == Execution::Native && !item.invoke) return false;
    by_name_.emplace(item.name, item.id);
    items_.emplace(item.id, item);
    return true;
  }
  const Item* Find(Id id) const {
    const auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
  }
  const Item* Find(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : Find(it->second);
  }
  bool Dispatch(Id id, Context* context) const {
    const auto* item = Find(id);
    return context && item && item->invoke && item->invoke(context);
  }
  const std::map<Id, Item>& Items() const { return items_; }
private:
  std::map<Id, Item> items_;
  std::map<std::string, Id> by_name_;
};
}  // namespace GekkoAOT::HLE
