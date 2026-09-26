#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone API-compatible DSP state used to compile the GPL-2.0-or-later
// Dolphin DSP instruction semantics without linking Dolphin Core.

#include "Common/CommonTypes.h"
#include <array>
#include <cstddef>

namespace DSP {

class DSPCore;

constexpr u32 DSP_IRAM_SIZE = 0x1000;
constexpr u32 DSP_IRAM_MASK = 0x0fff;
constexpr u32 DSP_IROM_SIZE = 0x1000;
constexpr u32 DSP_IROM_MASK = 0x0fff;
constexpr u32 DSP_DRAM_SIZE = 0x1000;
constexpr u32 DSP_DRAM_MASK = 0x0fff;
constexpr u32 DSP_COEF_SIZE = 0x0800;
constexpr u32 DSP_COEF_MASK = 0x07ff;
constexpr u16 DSP_RESET_VECTOR = 0x8000;
constexpr u8 DSP_STACK_DEPTH = 0x20;
constexpr u8 DSP_STACK_MASK = 0x1f;

enum : int {
  DSP_REG_AR0=0x00, DSP_REG_AR1=0x01, DSP_REG_AR2=0x02, DSP_REG_AR3=0x03,
  DSP_REG_IX0=0x04, DSP_REG_IX1=0x05, DSP_REG_IX2=0x06, DSP_REG_IX3=0x07,
  DSP_REG_WR0=0x08, DSP_REG_WR1=0x09, DSP_REG_WR2=0x0a, DSP_REG_WR3=0x0b,
  DSP_REG_ST0=0x0c, DSP_REG_ST1=0x0d, DSP_REG_ST2=0x0e, DSP_REG_ST3=0x0f,
  DSP_REG_ACH0=0x10, DSP_REG_ACH1=0x11, DSP_REG_CR=0x12, DSP_REG_SR=0x13,
  DSP_REG_PRODL=0x14, DSP_REG_PRODM=0x15, DSP_REG_PRODH=0x16, DSP_REG_PRODM2=0x17,
  DSP_REG_AXL0=0x18, DSP_REG_AXL1=0x19, DSP_REG_AXH0=0x1a, DSP_REG_AXH1=0x1b,
  DSP_REG_ACL0=0x1c, DSP_REG_ACL1=0x1d, DSP_REG_ACM0=0x1e, DSP_REG_ACM1=0x1f,
  DSP_REG_ACC0_FULL=0x20, DSP_REG_ACC1_FULL=0x21,
  DSP_REG_AX0_FULL=0x22, DSP_REG_AX1_FULL=0x23,
};

enum : u16 {
  CR_RESET=0x0001, CR_EXTERNAL_INT=0x0002, CR_HALT=0x0004,
  CR_INIT_CODE=0x0400, CR_INIT=0x0800,
};

enum : u16 {
  SR_CARRY=0x0001, SR_OVERFLOW=0x0002, SR_ARITH_ZERO=0x0004, SR_SIGN=0x0008,
  SR_OVER_S32=0x0010, SR_TOP2BITS=0x0020, SR_LOGIC_ZERO=0x0040,
  SR_OVERFLOW_STICKY=0x0080, SR_100=0x0100, SR_INT_ENABLE=0x0200,
  SR_400=0x0400, SR_EXT_INT_ENABLE=0x0800, SR_1000=0x1000,
  SR_MUL_MODIFY=0x2000, SR_40_MODE_BIT=0x4000, SR_MUL_UNSIGNED=0x8000,
  SR_CMP_MASK=0x003f,
};

enum class StackRegister { Call, Data, LoopAddress, LoopCounter };
enum class ExceptionType : u8 {
  StackOverflow=1, EXP_2=2, AcceleratorRawReadOverflow=3,
  AcceleratorRawWriteOverflow=4, AcceleratorSampleReadOverflow=5,
  EXP_6=6, ExternalInterrupt=7
};

class HostInterface {
public:
  virtual ~HostInterface() = default;
  virtual u16 ReadCpuMailboxHigh() = 0;
  virtual u16 ReadCpuMailboxLow() = 0;
  virtual void WriteDspMailboxHigh(u16 value) = 0;
  virtual void WriteDspMailboxLow(u16 value) = 0;
  virtual void RequestDspInterrupt() = 0;
  virtual bool ReadMain(u32 address, u8* dst, u32 size) = 0;
  virtual bool WriteMain(u32 address, const u8* src, u32 size) = 0;
  virtual u8 ReadAram(u32 address) = 0;
  virtual void WriteAram(u32 address, u8 value) = 0;
};

struct DSP_Regs {
  u16 ar[4]{}; u16 ix[4]{}; u16 wr[4]{}; u16 st[4]{};
  u16 cr=0; u16 sr=0;
  union Product { u64 val; struct { u16 l,m,h,m2; }; Product():val(0){} } prod;
  union AX { u32 val; struct { u16 l,h; }; AX():val(0){} } ax[2];
  union AC { u64 val; struct { u16 l,m; u32 h; }; AC():val(0){} } ac[2];
};

struct SDSP {
  explicit SDSP(DSPCore& core);
  void Reset();
  u16 ReadIMEM(u16 address) const;
  u16 ReadDMEM(u16 address);
  void WriteDMEM(u16 address, u16 value);
  u16 FetchInstruction();
  u16 PeekInstruction() const;
  void SkipInstruction();
  void StoreStack(StackRegister stack, u16 value);
  u16 PopStack(StackRegister stack);
  void SetSRFlag(u16 flag) { r.sr |= flag; }
  bool IsSRFlagSet(u16 flag) const { return (r.sr & flag) != 0; }
  void SetException(ExceptionType type);
  bool CheckExceptions();
  u16 ReadIFX(u16 address);
  void WriteIFX(u32 address, u16 value);
  void DoDMA();

  DSPCore& core;
  DSP_Regs r{};
  u16 pc=0;
  u16 control_reg=CR_HALT;
  u8 reg_stack_ptrs[4]{};
  u8 exceptions=0;
  u16 reg_stacks[4][DSP_STACK_DEPTH]{};
  std::array<u16,DSP_IRAM_SIZE> iram{};
  std::array<u16,DSP_DRAM_SIZE> dram{};
  std::array<u16,DSP_IROM_SIZE> irom{};
  std::array<u16,DSP_COEF_SIZE> coef{};
  std::array<u16,256> ifx{};

  // DSP accelerator state.
  u32 acc_start=0, acc_end=0, acc_current=0;
  u16 acc_format=0, acc_pred_scale=0, acc_input=0;
  s16 acc_gain=0, acc_yn1=0, acc_yn2=0;
  bool acc_reads_stopped=false;
};

class DSPCore {
public:
  explicit DSPCore(HostInterface* host=nullptr) : m_host(host), m_state(*this) {}
  SDSP& DSPState() { return m_state; }
  const SDSP& DSPState() const { return m_state; }
  HostInterface* Host() const { return m_host; }
  void SetHost(HostInterface* host) { m_host=host; }
  void Reset() { m_state.Reset(); }
  bool CheckExceptions() { return m_state.CheckExceptions(); }
  void CheckExternalInterrupt() {
    if (!m_state.IsSRFlagSet(SR_EXT_INT_ENABLE)) return;
    m_state.SetException(ExceptionType::ExternalInterrupt);
    m_state.control_reg &= static_cast<u16>(~CR_EXTERNAL_INT);
  }
private:
  HostInterface* m_host=nullptr;
  SDSP m_state;
};

} // namespace DSP
