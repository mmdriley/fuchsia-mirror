// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_INPUT_DRIVERS_HID_INPUT_REPORT_INSTANCE_H_
#define SRC_UI_INPUT_DRIVERS_HID_INPUT_REPORT_INSTANCE_H_

#include <lib/fidl/llcpp/buffer_then_heap_allocator.h>

#include <array>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>
#include <fbl/ring_buffer.h>

#include "src/ui/input/lib/hid-input-report/descriptors.h"
#include "src/ui/input/lib/hid-input-report/device.h"
#include "src/ui/input/lib/hid-input-report/fidl.h"
#include "src/ui/input/lib/hid-input-report/mouse.h"

namespace hid_input_report_dev {

namespace fuchsia_input_report = ::llcpp::fuchsia::input::report;

class InputReportInstance;

class InputReportBase {
 public:
  virtual void RemoveInstanceFromList(InputReportInstance* instance) = 0;
  virtual void CreateDescriptor(fidl::Allocator* allocator,
                                fuchsia_input_report::DeviceDescriptor::Builder* descriptor) = 0;
  virtual zx_status_t SendOutputReport(fuchsia_input_report::OutputReport report) = 0;
};

using InstanceDeviceType = ddk::Device<InputReportInstance, ddk::Closable, ddk::Messageable>;

class InputReportInstance : public InstanceDeviceType,
                            fuchsia_input_report::InputDevice::Interface,
                            public fbl::DoublyLinkedListable<InputReportInstance*> {
 public:
  InputReportInstance(zx_device_t* parent, uint32_t instance_id)
      : InstanceDeviceType(parent), instance_id_(instance_id) {}

  // The |InputReportBase| is responsible for creating |InputReportInstance| and adding it to
  // the LinkedList of instances that are owned by the base. The Instance is a child driver
  // of the base and can not outlive the base. The Instance driver must remove itself from
  // the LinkedList of it's Base driver during DdkClose.
  zx_status_t Bind(InputReportBase* base);

  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);
  void DdkRelease() { delete this; }
  zx_status_t DdkClose(uint32_t flags);

  void ReceiveReport(const uint8_t* report, size_t report_size, zx_time_t time,
                     hid_input_report::Device* device);

  // FIDL functions.
  void GetReportsEvent(GetReportsEventCompleter::Sync completer);
  void GetReports(GetReportsCompleter::Sync completer);
  void GetDescriptor(GetDescriptorCompleter::Sync completer);
  void SendOutputReport(::llcpp::fuchsia::input::report::OutputReport report,
                        SendOutputReportCompleter::Sync completer);

 private:
  // This is the static size that is used to allocate this instance's InputReports that
  // are stored in `reports_data`. This amount of memory is allocated with the driver
  // when the driver is initialized. If the `InputReports` go over this limit the
  // rest of the memory will be heap allocated as unique pointers.
  static constexpr size_t kFidlReportBufferSize = 4096 * 4;
  // This is the static size that is used to allocate this instance's InputDescriptor.
  // This amount of memory is stack allocated when a client calls GetDescriptor.
  static constexpr size_t kFidlDescriptorBufferSize = 4096 * 2;

  uint32_t instance_id_;

  fbl::Mutex report_lock_;
  zx::event reports_event_ __TA_GUARDED(report_lock_);

  fidl::BufferThenHeapAllocator<kFidlReportBufferSize> report_allocator_ __TA_GUARDED(report_lock_);
  fbl::RingBuffer<fuchsia_input_report::InputReport, fuchsia_input_report::MAX_DEVICE_REPORT_COUNT>
      reports_data_ __TA_GUARDED(report_lock_);

  InputReportBase* base_ = nullptr;
};

}  // namespace hid_input_report_dev

#endif  // SRC_UI_INPUT_DRIVERS_HID_INPUT_REPORT_INSTANCE_H_