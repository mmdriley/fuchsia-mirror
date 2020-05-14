// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-xhci.h"

#include <lib/device-protocol/pdev.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/sync/completion.h>
#include <lib/zx/bti.h>
#include <lib/zx/vmar.h>

#include <list>

#include <fake-dma-buffer/fake-dma-buffer.h>
#include <fake-mmio-reg/fake-mmio-reg.h>

namespace usb_xhci {

const zx::bti kFakeBti(42);

struct FakeTRB;
struct FakePhysAddr {
  uint64_t magic;
  FakeTRB* value;
};

struct FakeTRB : TRB {
  FakeTRB() {
    phys_addr_ = std::make_unique<FakePhysAddr>();
    phys_addr_->magic = kMagicValue;
    phys_addr_->value = this;
  }

  zx_paddr_t phys() { return reinterpret_cast<zx_paddr_t>(phys_addr_.get()); }

  static bool is_valid_paddr(zx_paddr_t phys) {
    return *reinterpret_cast<uint64_t*>(phys) == kMagicValue;
  }

  static FakeTRB* get(zx_paddr_t phys) {
    if (!is_valid_paddr(phys)) {
      return nullptr;
    }
    FakePhysAddr* addr = reinterpret_cast<FakePhysAddr*>(phys);
    return addr->value;
  }

  ~FakeTRB() {
    // Cast to volatile to prevent the compiler
    // from optimizing out this zero operation.
    *reinterpret_cast<volatile uint64_t*>(phys_addr_.get()) = 0;
  }

  static std::unique_ptr<FakeTRB> FromTRB(TRB* trb) {
    return std::unique_ptr<FakeTRB>(static_cast<FakeTRB*>(trb));
  }

  // Magic value to use for determining if a physical address is valid or not.
  // ASAN builds should also trigger an error if we try dereferencing something
  // that isn't valid. This value is a fallback for cases where ASAN isn't being used.
  static constexpr uint64_t kMagicValue = 0x12345678901ABCDEU;
  std::unique_ptr<FakePhysAddr> phys_addr_;
  zx_paddr_t next = 0;
  zx_paddr_t prev = 0;
};

class FakeDevice : public ddk::PDevProtocol<FakeDevice>, public ddk::PciProtocol<FakeDevice> {
 public:
  FakeDevice() : pdev_({&pdev_protocol_ops_, this}), pci_({&pci_protocol_ops_, this}) {
    constexpr auto kHcsParams2 = 2;
    constexpr auto kHccParams1 = 4;
    constexpr auto kXecp = 320;
    constexpr auto kOffset = 0;
    constexpr auto kHcsParams1 = 1;
    constexpr auto kOffset1 = 5;
    constexpr auto kOffset2 = 6;
    constexpr auto kUsbCmd = 7;
    constexpr auto kUsbSts = 8;
    constexpr auto kUsbPageSize = 9;
    constexpr auto kConfig = 14;
    constexpr auto kCrCr = 13;
    constexpr auto kDcbaa = 19;
    constexpr auto kDoorbellBase = 1024;
    constexpr auto kImodi = 457;

    regs_[kHcsParams2].SetReadCallback([]() {
      auto params = HCSPARAMS2::Get().FromValue(0);
      params.set_ERST_MAX(4);
      params.set_MAX_SCRATCHPAD_BUFFERS_LOW(1);
      return params.reg_value();
    });

    regs_[kHccParams1].SetReadCallback([]() {
      auto params = HCCPARAMS1::Get().FromValue(0);
      params.set_AC64(true);
      params.set_CSZ(true);
      params.set_xECP(320);
      return params.reg_value();
    });

    regs_[kXecp].SetReadCallback([=]() {
      auto xecp = XECP::Get(HCCPARAMS1::Get().FromValue(static_cast<uint32_t>(regs_[4].Read())))
                      .FromValue(0);
      xecp.set_NEXT(0);
      xecp.set_ID(XECP::UsbLegacySupport);
      if (driver_owned_controller_) {
        xecp.set_reg_value(xecp.reg_value() | 1 << 24);
      } else {
        xecp.set_reg_value(xecp.reg_value() | 1 << 16);
      }
      return xecp.reg_value();
    });

    regs_[kXecp].SetWriteCallback([=](uint64_t value) {
      if (value & (1 << 24)) {
        driver_owned_controller_ = true;
      }
    });
    regs_[kOffset].SetReadCallback([=]() { return 0x1c; });

    regs_[kHcsParams1].SetReadCallback([=]() {
      HCSPARAMS1 parms = HCSPARAMS1::Get().FromValue(0);
      parms.set_MaxIntrs(1);
      parms.set_MaxPorts(4);
      parms.set_MaxSlots(32);
      return parms.reg_value();
    });

    regs_[kOffset1].SetReadCallback([=]() { return 0x1000; });

    regs_[kOffset2].SetReadCallback([=]() { return 0x700; });

    regs_[kUsbCmd].SetReadCallback([=]() {
      USBCMD cmd = USBCMD::Get(static_cast<uint8_t>(regs_[0].Read())).FromValue(0);
      cmd.set_ENABLE(controller_enabled_);
      cmd.set_EWE(event_wrap_enable_);
      cmd.set_HSEE(host_system_error_enable_);
      cmd.set_INTE(irq_enable_);
      cmd.set_RESET(0);
      return cmd.reg_value();
    });

    regs_[kUsbCmd].SetWriteCallback([=](uint64_t value) {
      USBCMD cmd = USBCMD::Get(static_cast<uint8_t>(regs_[0].Read()))
                       .FromValue(static_cast<uint32_t>(value));
      if (cmd.RESET()) {
        controller_was_reset_ = true;
      }
      controller_enabled_ = cmd.ENABLE();
      event_wrap_enable_ = cmd.EWE();
      host_system_error_enable_ = cmd.HSEE();
      irq_enable_ = cmd.INTE();
    });

    regs_[kUsbSts].SetReadCallback([=]() {
      auto sts = USBSTS::Get(0x1c).FromValue(0);
      sts.set_HCHalted(!controller_enabled_);
      return sts.reg_value();
    });

    regs_[kUsbPageSize].SetReadCallback([=]() {
      USB_PAGESIZE size = USB_PAGESIZE::Get(0x1c).FromValue(0);
      size.set_PageSize(1);
      return size.reg_value();
    });

    regs_[kConfig].SetReadCallback([=]() {
      CONFIG config = CONFIG::Get(0x1c).FromValue(0);
      config.set_MaxSlotsEn(slots_enabled_);
      return config.reg_value();
    });

    regs_[kConfig].SetWriteCallback([=](uint64_t value) {
      CONFIG config = CONFIG::Get(0x1c).FromValue(static_cast<uint32_t>(value));
      slots_enabled_ = config.MaxSlotsEn();
    });

    regs_[kCrCr].SetWriteCallback([=](uint64_t value) {
      CRCR cr = CRCR::Get(0x1c).FromValue(value);
      crcr_ = reinterpret_cast<zx_paddr_t>(cr.PTR());
    });

    regs_[kDcbaa].SetReadCallback([=]() { return dcbaa_; });
    regs_[kDcbaa].SetWriteCallback([=](uint64_t value) {
      auto val = DCBAAP::Get(0x1c).FromValue(value);
      dcbaa_ = val.PTR();
    });
    doorbell_callback_ = [](uint8_t doorbell, uint8_t target) {};
    for (size_t i = 0; i < 32; i++) {
      regs_[kDoorbellBase + i].SetWriteCallback([=](uint64_t value) {
        auto buffer = mmio();
        auto bell = DOORBELL::Get(DoorbellOffset::Get().ReadFrom(&buffer), 0)
                        .FromValue(static_cast<uint32_t>(value));
        doorbell_callback_(static_cast<uint8_t>(i), static_cast<uint8_t>(bell.Target()));
      });
    }
    regs_[kImodi].SetWriteCallback([=](uint64_t value) {
      auto buffer = mmio();
      auto imodi = IMODI::Get(RuntimeRegisterOffset::Get().ReadFrom(&buffer), 0)
                       .FromValue(static_cast<uint32_t>(value));
      imodi_ = static_cast<uint16_t>(imodi.MODI());
    });

    region_.emplace(regs_, sizeof(uint32_t), fbl::count_of(regs_));
    // Control register
    zx::interrupt::create(zx::resource(ZX_HANDLE_INVALID), 0, ZX_INTERRUPT_VIRTUAL, &irq_);
  }

  const pdev_protocol_t* pdev() const { return &pdev_; }
  const pci_protocol_t* pci() const { return &pci_; }

  zx_status_t PDevGetMmio(uint32_t index, pdev_mmio_t* out_mmio) {
    out_mmio->offset = reinterpret_cast<size_t>(this);
    return ZX_OK;
  }

  ddk::MmioBuffer mmio() { return ddk::MmioBuffer(region_->GetMmioBuffer()); }

  zx_status_t PDevGetInterrupt(uint32_t index, uint32_t flags, zx::interrupt* out_irq) {
    irq_signaller_ = zx::unowned_interrupt(irq_);
    *out_irq = std::move(irq_);
    return ZX_OK;
  }

  zx_status_t PDevGetBti(uint32_t index, zx::bti* out_bti) {
    *out_bti = zx::bti(kFakeBti.get());
    return ZX_OK;
  }

  zx_status_t PDevGetSmc(uint32_t index, zx::resource* out_resource) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PDevGetDeviceInfo(pdev_device_info_t* out_info) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PDevGetBoardInfo(pdev_board_info_t* out_info) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciGetBar(uint32_t bar_id, zx_pci_bar_t* out_res) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciEnableBusMaster(bool enable) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciResetDevice() { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciMapInterrupt(zx_status_t which_irq, zx::interrupt* out_handle) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciQueryIrqMode(zx_pci_irq_mode_t mode, uint32_t* out_max_irqs) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciSetIrqMode(zx_pci_irq_mode_t mode, uint32_t requested_irq_count) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciGetDeviceInfo(zx_pcie_device_info_t* out_into) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciConfigRead8(uint16_t offset, uint8_t* out_value) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciConfigRead16(uint16_t offset, uint16_t* out_value) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciConfigRead32(uint16_t offset, uint32_t* out_value) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciConfigWrite8(uint16_t offset, uint8_t value) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciConfigWrite16(uint16_t offset, uint16_t value) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciConfigWrite32(uint16_t offset, uint32_t value) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t PciGetFirstCapability(uint8_t cap_id, uint8_t* out_offset) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciGetNextCapability(uint8_t cap_id, uint8_t offset, uint8_t* out_offset) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciGetFirstExtendedCapability(uint16_t cap_id, uint16_t* out_offset) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciGetNextExtendedCapability(uint16_t cap_id, uint16_t offset, uint16_t* out_offset) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciGetAuxdata(const char* args, void* out_data_buffer, size_t data_size,
                            size_t* out_data_actual) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t PciGetBti(uint32_t index, zx::bti* out_bti) { return ZX_ERR_NOT_SUPPORTED; }

  void SetDoorbellCallback(fit::function<void(uint8_t, uint8_t)> callback) {
    doorbell_callback_ = std::move(callback);
  }

  FakeTRB* crcr() { return FakeTRB::get(crcr_); }

 private:
  ddk_fake::FakeMmioReg regs_[2048];
  std::optional<ddk_fake::FakeMmioRegRegion> region_;
  pdev_protocol_t pdev_;
  pci_protocol_t pci_;
  zx::interrupt irq_;
  zx::unowned_interrupt irq_signaller_;
  bool driver_owned_controller_ = false;
  bool controller_enabled_ = false;
  bool controller_was_reset_ = false;
  bool event_wrap_enable_ = false;
  bool irq_enable_ = false;
  bool host_system_error_enable_ = false;
  uint32_t slots_enabled_ = 0;
  zx_paddr_t crcr_ = 0;
  zx_paddr_t dcbaa_ = 0;
  uint16_t imodi_;
  fit::function<void(uint8_t, uint8_t)> doorbell_callback_;
};

struct FakeUsbDevice {
  uint32_t device_id;
  uint32_t hub_id;
  usb_speed_t speed;
  bool fake_root_hub;
};

class Ddk : public fake_ddk::Bind, public ddk::UsbBusInterfaceProtocol<Ddk> {
 public:
  Ddk() {}
  bool added() { return add_called_; }
  const device_add_args_t& args() { return add_args_; }
  bool visible() { return make_visible_called_; }

  void reset() { sync_completion_reset(&completion_); }

  void wait() { sync_completion_wait(&completion_, ZX_TIME_INFINITE); }

  zx_status_t UsbBusInterfaceAddDevice(uint32_t device_id, uint32_t hub_id, usb_speed_t speed) {
    FakeUsbDevice fake_device;
    fake_device.device_id = device_id;
    fake_device.hub_id = hub_id;
    fake_device.speed = speed;
    fake_device.fake_root_hub = device_id >= 32;
    devices_[device_id] = fake_device;
    sync_completion_signal(&completion_);
    return ZX_OK;
  }

  zx_status_t UsbBusInterfaceRemoveDevice(uint32_t device_id) {
    devices_.erase(device_id);
    return ZX_OK;
  }

  zx_status_t UsbBusInterfaceResetPort(uint32_t hub_id, uint32_t port, bool enumerating) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t UsbBusInterfaceReinitializeDevice(uint32_t device_id) { return ZX_ERR_NOT_SUPPORTED; }
  const std::map<uint32_t, FakeUsbDevice>& devices() { return devices_; }

 private:
  zx_status_t DeviceAdd(zx_driver_t* drv, zx_device_t* parent, device_add_args_t* args,
                        zx_device_t** out) override {
    zx_status_t status = fake_ddk::Bind::DeviceAdd(drv, parent, args, out);
    if (status != ZX_OK) {
      return status;
    }
    sync_completion_signal(&completion_);
    add_args_ = *args;
    return ZX_OK;
  }

  void DeviceMakeVisible(zx_device_t* device) override {
    usb_bus_interface_protocol_t proto;
    proto.ctx = this;
    proto.ops = &usb_bus_interface_protocol_ops_;
    static_cast<UsbXhci*>(add_args_.ctx)->UsbHciSetBusInterface(&proto);
    make_visible_called_ = true;
  }
  std::map<uint32_t, FakeUsbDevice> devices_;
  sync_completion_t completion_;
  device_add_args_t add_args_;
};

using TestRequest = usb::CallbackRequest<sizeof(max_align_t)>;
class XhciHarness : public zxtest::Test {
 public:
  FakeTRB* CreateTRB() {
    auto it = trbs_.insert(trbs_.end(), std::make_unique<FakeTRB>());
    (*it)->control = 0;
    (*it)->ptr = 0;
    (*it)->status = 0;
    return it->get();
  }

  size_t GetMaxDeviceCount() { return device_->UsbHciGetMaxDeviceCount(); }

  void RequestQueue(TestRequest request) { request.Queue(*device_); }

  template <typename Callback>
  zx_status_t AllocateRequest(std::optional<TestRequest>* request, uint32_t device_id,
                              uint64_t data_size, uint8_t endpoint, Callback callback) {
    zx_status_t result = TestRequest::Alloc(request, data_size, endpoint,
                                            device_->UsbHciGetRequestSize(), std::move(callback));
    if (result != ZX_OK) {
      return result;
    }
    void* virt;
    (*request)->Mmap(&virt);
    static_assert(sizeof(uint64_t) == sizeof(void*));
    size_t phys_count = fbl::round_up(data_size, ZX_PAGE_SIZE) / ZX_PAGE_SIZE;
    (*request)->request()->phys_count = phys_count;
    // Need to use malloc for compatibility with the C ABI (which will eventually call free)
    (*request)->request()->phys_list =
        static_cast<zx_paddr_t*>(malloc(sizeof(zx_paddr_t) * phys_count));
    for (size_t i = 0; i < phys_count; i++) {
      auto trb = CreateTRB();
      trb->ptr = reinterpret_cast<uint64_t>(virt) + (ZX_PAGE_SIZE * i);
      (*request)->request()->phys_list[i] = trb->phys();
    }
    (*request)->request()->header.device_id = device_id;
    return ZX_OK;
  }

  uint8_t AllocateSlot() {
    if (slot_freelist_.empty()) {
      slot_id_++;
      return slot_id_;
    }
    uint8_t retval = slot_freelist_.back();
    slot_freelist_.pop_back();
    return retval;
  }

  FakeUsbDevice ConnectDevice(uint8_t port, usb_speed_t speed) {
    std::optional<HubInfo> hub;
    uint8_t slot = AllocateSlot();
    device_->get_port_state()[port - 1].is_connected = true;
    device_->get_port_state()[port - 1].link_active = true;
    device_->get_port_state()[port - 1].slot_id = slot;
    device_->SetDeviceInformation(slot, slot, hub);
    device_->AddressDeviceCommand(slot, port, hub, true);
    ddk_.reset();
    device_->DeviceOnline(slot, port, speed);
    ddk_.wait();
    return ddk_.devices().find(slot - 1)->second;
  }

  void EnableEndpoint(uint32_t device_id, uint8_t ep_num, bool is_in_endpoint) {
    usb_endpoint_descriptor_t ep_desc;
    ep_desc.bEndpointAddress = ep_num | (is_in_endpoint ? 0x80 : 0);
    device_->UsbHciEnableEndpoint(device_id, &ep_desc, nullptr);
  }

  void SetDoorbellListener(fit::function<void(uint8_t, uint8_t)> listener) {
    pdev_.SetDoorbellCallback(std::move(listener));
  }

  FakeTRB* crcr() { return pdev_.crcr(); }

  virtual ~XhciHarness() {}

 protected:
  std::unique_ptr<UsbXhci> device_;
  Ddk ddk_;
  FakeDevice pdev_;

 private:
  std::vector<uint8_t> slot_freelist_;
  uint8_t slot_id_ = 0;
  std::list<std::unique_ptr<FakeTRB>> trbs_;
};

class XhciMmioHarness : public XhciHarness {
 public:
  void SetUp() override {
    zx::interrupt interrupt;
    ASSERT_OK(zx::interrupt::create({}, 0, ZX_INTERRUPT_VIRTUAL, &interrupt));
    {
      fbl::Array<fake_ddk::ProtocolEntry> protocols(new fake_ddk::ProtocolEntry[1], 1);
      protocols[0] = {ZX_PROTOCOL_PDEV, {pdev_.pdev()->ops, pdev_.pdev()->ctx}};
      ddk_.SetProtocols(std::move(protocols));
    }
    auto dev = std::make_unique<UsbXhci>(fake_ddk::kFakeParent);
    dev->set_test_harness(this);
    dev->DdkAdd("xhci");
    dev->InitThread(ddk_fake::CreateBufferFactory());
    ASSERT_TRUE(ddk_.added());
    ASSERT_TRUE(ddk_.visible());
    dev.release();
    device_.reset(static_cast<UsbXhci*>(ddk_.args().ctx));
  }

  void TearDown() override {
    auto device = device_.release();
    ddk::UnbindTxn txn(device->zxdev());
    device->DdkUnbindNew(std::move(txn));
    ASSERT_OK(ddk_.WaitUntilRemove());
  }
};

fbl::DoublyLinkedList<std::unique_ptr<TRBContext>> TransferRing::TakePendingTRBs() {
  fbl::AutoLock al(&mutex_);
  return std::move(pending_trbs_);
}

void EventRing::ScheduleTask(fit::promise<TRB*, zx_status_t> promise) {
  auto continuation = promise.then([=](fit::result<TRB*, zx_status_t>& result) {
    if (result.is_error()) {
      if (result.error() == ZX_ERR_BAD_STATE) {
        hci_->Shutdown(ZX_ERR_BAD_STATE);
      }
    }
    return result;
  });
  executor_.schedule_task(std::move(continuation));
}

void EventRing::RunUntilIdle() { executor_.run(); }

zx_status_t TransferRing::AllocateTRB(TRB** trb, State* state) {
  fbl::AutoLock _(&mutex_);
  if (state) {
    state->pcs = pcs_;
    state->trbs = trbs_;
  }
  auto new_trb = static_cast<XhciHarness*>(hci_->get_test_harness())->CreateTRB();
  new_trb->prev = static_cast<FakeTRB*>(trbs_)->phys();
  static_cast<FakeTRB*>(trbs_)->next = new_trb->phys();
  trbs_ = new_trb;
  trbs_->ptr = 0;
  trbs_->status = pcs_;
  *trb = trbs_;
  return ZX_OK;
}

zx_status_t TransferRing::CompleteTRB(TRB* trb, std::unique_ptr<TRBContext>* context) {
  fbl::AutoLock _(&mutex_);
  if (pending_trbs_.is_empty()) {
    return ZX_ERR_CANCELED;
  }
  dequeue_trb_ = trb;
  *context = pending_trbs_.pop_front();
  if (trb != (*context)->trb) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

void TransferRing::CommitTransaction(const State& start) {}

zx_status_t TransferRing::AssignContext(TRB* trb, std::unique_ptr<TRBContext> context, TRB* first) {
  fbl::AutoLock _(&mutex_);
  if (context->token != token_) {
    return ZX_ERR_INVALID_ARGS;
  }
  context->trb = trb;
  pending_trbs_.push_back(std::move(context));
  return ZX_OK;
}

zx_status_t xhci_start_root_hubs(UsbXhci* xhci) { return ZX_OK; }

zx_status_t TransferRing::Init(size_t page_size, const zx::bti& bti, EventRing* ring, bool is_32bit,
                               ddk::MmioBuffer* mmio, const UsbXhci& hci) {
  fbl::AutoLock _(&mutex_);
  if (trbs_ != nullptr) {
    return ZX_ERR_BAD_STATE;
  }
  page_size_ = page_size;
  bti_ = &bti;
  ring_ = ring;
  is_32_bit_ = is_32bit;
  mmio_ = mmio;
  isochronous_ = false;
  token_++;
  hci_ = &hci;
  trbs_ = static_cast<XhciHarness*>(hci_->get_test_harness())->CreateTRB();
  static_assert(sizeof(uint64_t) == sizeof(this));
  trbs_->ptr = reinterpret_cast<uint64_t>(this);
  trbs_->status = pcs_;
  trb_start_phys_ = static_cast<FakeTRB*>(trbs_)->phys();
  return ZX_OK;
}

CRCR TransferRing::TransferRing::phys(uint8_t cap_length) __TA_NO_THREAD_SAFETY_ANALYSIS {
  CRCR cr = CRCR::Get(cap_length).FromValue(trb_start_phys_);
  assert(trb_start_phys_);
  cr.set_RCS(pcs_);
  return cr;
}

TransferRing::State TransferRing::SaveState() {
  fbl::AutoLock _(&mutex_);
  State state;
  state.pcs = pcs_;
  state.trbs = trbs_;
  return state;
}

void TransferRing::Restore(const State& state) {
  fbl::AutoLock _(&mutex_);
  trbs_ = state.trbs;
  pcs_ = state.pcs;
}

zx_status_t TransferRing::AddTRB(const TRB& trb, std::unique_ptr<TRBContext> context) {
  fbl::AutoLock _(&mutex_);
  if (context->token != token_) {
    return ZX_ERR_INVALID_ARGS;
  }
  FakeTRB* alloc_trb;
  alloc_trb = static_cast<XhciHarness*>(hci_->get_test_harness())->CreateTRB();
  alloc_trb->prev = static_cast<FakeTRB*>(trbs_)->phys();
  static_cast<FakeTRB*>(trbs_)->next = alloc_trb->phys();
  trbs_ = alloc_trb;
  alloc_trb->control = trb.control;
  alloc_trb->ptr = trb.ptr;
  alloc_trb->status = trb.status;
  context->token = token_;
  context->trb = alloc_trb;
  pending_trbs_.push_back(std::move(context));
  return ZX_OK;
}

zx_status_t TransferRing::Deinit() {
  fbl::AutoLock _(&mutex_);
  if (!trbs_) {
    return ZX_ERR_BAD_STATE;
  }
  trbs_ = nullptr;
  dequeue_trb_ = nullptr;
  pcs_ = true;
  return ZX_OK;
}

zx_status_t TransferRing::DeinitIfActive() __TA_NO_THREAD_SAFETY_ANALYSIS {
  if (trbs_) {
    return Deinit();
  }
  return ZX_OK;
}

zx_paddr_t TransferRing::VirtToPhys(TRB* trb) { return static_cast<FakeTRB*>(trb)->phys(); }

zx_status_t EventRingSegmentTable::Init(size_t page_size, const zx::bti& bti, bool is_32bit,
                                        uint32_t erst_max, ERSTSZ erst_size,
                                        const dma_buffer::BufferFactory& factory,
                                        ddk::MmioBuffer* mmio) {
  erst_size_ = erst_size;
  bti_ = &bti;
  page_size_ = page_size;
  is_32bit_ = is_32bit;
  mmio_.emplace(mmio->View(0));
  zx_status_t status = factory.CreatePaged(bti, ZX_PAGE_SIZE, false, &erst_);
  if (status != ZX_OK) {
    return status;
  }

  count_ = page_size / sizeof(ERSTEntry);
  if (count_ > erst_max) {
    count_ = erst_max;
  }
  entries_ = static_cast<ERSTEntry*>(erst_->virt());
  return ZX_OK;
}

zx_status_t EventRing::Init(size_t page_size, const zx::bti& bti, ddk::MmioBuffer* buffer,
                            bool is_32bit, uint32_t erst_max, ERSTSZ erst_size, ERDP erdp_reg,
                            IMAN iman_reg, uint8_t cap_length, HCSPARAMS1 hcs_params_1,
                            CommandRing* command_ring, DoorbellOffset doorbell_offset, UsbXhci* hci,
                            HCCPARAMS1 hcc_params_1, uint64_t* dcbaa) {
  fbl::AutoLock _(&segment_mutex_);
  erdp_reg_ = erdp_reg;
  hcs_params_1_ = hcs_params_1;
  mmio_ = buffer;
  bti_ = &bti;
  page_size_ = page_size;
  is_32bit_ = is_32bit;
  mmio_ = buffer;
  iman_reg_ = iman_reg;
  cap_length_ = cap_length;
  command_ring_ = command_ring;
  doorbell_offset_ = doorbell_offset;
  hci_ = hci;
  hcc_params_1_ = hcc_params_1;
  dcbaa_ = dcbaa;
  static_assert(sizeof(zx_paddr_t) == sizeof(this));
  erdp_phys_ = reinterpret_cast<zx_paddr_t>(this);
  return segments_.Init(page_size, bti, is_32bit, erst_max, erst_size, hci->buffer_factory(),
                        mmio_);
  ;
}

int Interrupter::IrqThread() { return 0; }

// Enumerates a device as specified in xHCI section 4.3 starting from step 4
// This method should be called once the physical port of a device has been
// initialized.
TRBPromise EnumerateDevice(UsbXhci* hci, uint8_t port, std::optional<HubInfo> hub_info) {
  fit::bridge<TRB*, zx_status_t> bridge;
  return bridge.consumer.promise();
}

struct alignas(4096) FakeVMO {
  size_t size;
  uint32_t alignment_log2;
  bool enable_cache;
  zx::vmo backing_storage;
  void* virt;
};

TEST_F(XhciMmioHarness, QueueControlRequest) {
  ConnectDevice(1, USB_SPEED_HIGH);
  bool rang = false;
  SetDoorbellListener([&](uint8_t doorbell, uint8_t target) {
    if (doorbell == 1 && target == 1) {
      rang = true;
    }
  });

  std::optional<TestRequest> request;
  bool invoked = false;
  AllocateRequest(&request, 0, sizeof(void*), 0, [&](TestRequest request) {
    invoked = true;
    void** parameters;
    request.Mmap(reinterpret_cast<void**>(&parameters));
    EXPECT_EQ(*parameters, parameters);
  });
  request->request()->setup.bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
  request->request()->setup.bRequest = USB_REQ_GET_DESCRIPTOR;
  request->request()->setup.wValue = USB_DT_DEVICE << 8;
  request->request()->setup.wLength = sizeof(void*);
  RequestQueue(std::move(*request));
  ASSERT_TRUE(rang);
  // Find slot context pointer in address device command
  auto cr = FakeTRB::get(crcr()->next);
  Control control_trb = Control::FromTRB(cr);
  ASSERT_EQ(control_trb.Type(), Control::AddressDeviceCommand);
  auto control = static_cast<unsigned char*>(reinterpret_cast<FakeVMO*>(cr->ptr)->virt);
  auto endpoint_context = reinterpret_cast<EndpointContext*>(control + (64 * 2));
  auto ring_phys =
      static_cast<zx_paddr_t>(static_cast<uint64_t>(endpoint_context->dequeue_pointer_a) |
                              (static_cast<uint64_t>(endpoint_context->dequeue_pointer_b) << 32)) &
      (~1);
  auto trb = FakeTRB::get(ring_phys);
  auto initial_trb = trb;
  // Setup
  trb = FakeTRB::get(trb->next);
  auto setup_trb = static_cast<Setup*>(static_cast<TRB*>(trb));
  ASSERT_EQ(setup_trb->length(), 8);
  ASSERT_EQ(setup_trb->IDT(), 1);
  ASSERT_EQ(setup_trb->TRT(), Setup::IN);
  // Data
  trb = FakeTRB::get(trb->next);
  auto data_trb = static_cast<ControlData*>(static_cast<TRB*>(trb));
  ASSERT_EQ(data_trb->DIRECTION(), 1);
  ASSERT_EQ(data_trb->INTERRUPTER(), 0);
  ASSERT_EQ(data_trb->LENGTH(), sizeof(void*));
  ASSERT_EQ(data_trb->SIZE(), 0);
  ASSERT_TRUE(data_trb->NO_SNOOP());
  void** virt = reinterpret_cast<void**>(FakeTRB::get(static_cast<zx_paddr_t>(data_trb->ptr))->ptr);
  *virt = virt;
  // Status
  trb = FakeTRB::get(trb->next);
  auto status_trb = static_cast<Status*>(static_cast<TRB*>(trb));
  ASSERT_EQ(status_trb->DIRECTION(), 0);
  ASSERT_EQ(status_trb->INTERRUPTER(), 0);
  ASSERT_TRUE(status_trb->IOC());
  // Interrupt on completion
  TransferRing* ring = reinterpret_cast<TransferRing*>(initial_trb->ptr);
  std::unique_ptr<TRBContext> context;
  ring->CompleteTRB(trb, &context);
  context->request->Complete(ZX_OK, sizeof(void*));
  ASSERT_TRUE(invoked);
}

TEST_F(XhciMmioHarness, QueueNormalRequest) {
  ConnectDevice(1, USB_SPEED_HIGH);
  EnableEndpoint(0, 1, true);
  bool rang = false;
  SetDoorbellListener([&](uint8_t doorbell, uint8_t target) {
    if (doorbell == 1 && target == 3) {
      rang = true;
    }
  });

  std::optional<TestRequest> request;
  bool invoked = false;
  AllocateRequest(&request, 0, ZX_PAGE_SIZE * 2, 1 | 0x80, [&](TestRequest request) {
    invoked = true;
    void** parameters;
    request.Mmap(reinterpret_cast<void**>(&parameters));
    EXPECT_EQ(*parameters, parameters);
  });

  RequestQueue(std::move(*request));

  ASSERT_TRUE(rang);
  // Find slot context pointer in address device command
  auto cr = FakeTRB::get(crcr()->next);
  Control control_trb = Control::FromTRB(cr);
  ASSERT_EQ(control_trb.Type(), Control::AddressDeviceCommand);
  auto control = static_cast<unsigned char*>(reinterpret_cast<FakeVMO*>(cr->ptr)->virt);
  auto endpoint_context = reinterpret_cast<EndpointContext*>(control + (64 * 4));
  auto ring_phys =
      static_cast<zx_paddr_t>(static_cast<uint64_t>(endpoint_context->dequeue_pointer_a) |
                              (static_cast<uint64_t>(endpoint_context->dequeue_pointer_b) << 32)) &
      (~1);

  auto trb = FakeTRB::get(ring_phys);
  auto first_trb = trb;

  // Data (page 0)
  trb = FakeTRB::get(trb->next);
  auto data_trb = static_cast<Normal*>(static_cast<TRB*>(trb));
  ASSERT_EQ(data_trb->IOC(), 0);
  ASSERT_EQ(data_trb->ISP(), 1);
  ASSERT_EQ(data_trb->INTERRUPTER(), 0);
  ASSERT_EQ(data_trb->LENGTH(), ZX_PAGE_SIZE);
  ASSERT_EQ(data_trb->SIZE(), 1);
  ASSERT_TRUE(data_trb->NO_SNOOP());
  void** virt = reinterpret_cast<void**>(FakeTRB::get(static_cast<zx_paddr_t>(data_trb->ptr))->ptr);
  *virt = virt;

  // Data (page 1)
  trb = FakeTRB::get(trb->next);
  data_trb = static_cast<Normal*>(static_cast<TRB*>(trb));
  ASSERT_EQ(data_trb->IOC(), 1);
  ASSERT_EQ(data_trb->ISP(), 0);
  ASSERT_EQ(data_trb->INTERRUPTER(), 0);
  ASSERT_EQ(data_trb->LENGTH(), ZX_PAGE_SIZE);
  ASSERT_EQ(data_trb->SIZE(), 0);
  ASSERT_TRUE(data_trb->NO_SNOOP());

  // Interrupt on completion
  TransferRing* ring = reinterpret_cast<TransferRing*>(first_trb->ptr);
  std::unique_ptr<TRBContext> context;
  ring->CompleteTRB(trb, &context);
  context->request->Complete(ZX_OK, sizeof(void*));
  ASSERT_TRUE(invoked);
}

TEST_F(XhciMmioHarness, GetMaxDeviceCount) { ASSERT_EQ(GetMaxDeviceCount(), 34); }

}  // namespace usb_xhci

zx_status_t ddk::PDev::MapMmio(uint32_t index, std::optional<MmioBuffer>* mmio) {
  pdev_mmio_t pdev_mmio;
  zx_status_t status = GetMmio(index, &pdev_mmio);
  if (status != ZX_OK) {
    return status;
  }
  auto* src = reinterpret_cast<usb_xhci::FakeDevice*>(pdev_mmio.offset);
  mmio->emplace(src->mmio());
  return ZX_OK;
}

zx_status_t ddk::Pci::MapMmio(uint32_t index, uint32_t cache_policy,
                              std::optional<MmioBuffer>* mmio) {
  return ZX_OK;
}