// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MSD_VSL_DEVICE_H
#define MSD_VSL_DEVICE_H

#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include "device_request.h"
#include "gpu_features.h"
#include "magma_util/macros.h"
#include "magma_util/register_io.h"
#include "magma_util/thread.h"
#include "magma_vsl_gc_types.h"
#include "mapped_batch.h"
#include "msd.h"
#include "msd_vsl_connection.h"
#include "page_table_arrays.h"
#include "page_table_slot_allocator.h"
#include "platform_bus_mapper.h"
#include "platform_device.h"
#include "platform_semaphore.h"
#include "ringbuffer.h"

class MsdVslDevice : public msd_device_t,
                     public AddressSpace::Owner,
                     public MsdVslConnection::Owner {
 public:
  using DeviceRequest = DeviceRequest<MsdVslDevice>;

  // Creates a device for the given |device_handle| and returns ownership.
  // If |start_device_thread| is false, then StartDeviceThread should be called
  // to enable device request processing.
  static std::unique_ptr<MsdVslDevice> Create(void* device_handle, bool start_device_thread);

  MsdVslDevice() { magic_ = kMagic; }

  virtual ~MsdVslDevice();

  uint32_t device_id() const { return device_id_; }
  uint32_t revision() const { return revision_; }

  bool IsIdle();
  bool StopRingbuffer();

  std::unique_ptr<MsdVslConnection> Open(msd_client_id_t client_id);

  magma_status_t ChipIdentity(magma_vsl_gc_chip_identity* out_identity);
  magma_status_t ChipOption(magma_vsl_gc_chip_option* out_option);

  struct DumpState {
    uint64_t max_completed_sequence_number;
    uint64_t next_sequence_number;
    bool idle;
    // This may be false if no batch has been submitted yet.
    bool page_table_arrays_enabled;
    uint32_t exec_addr;

    std::vector<MappedBatch*> inflight_batches;
  };

  void Dump(DumpState* dump_state);
  void DumpToString(std::vector<std::string>* dump_out);
  void DumpStatusToLog();

  std::vector<MappedBatch*> GetInflightBatches();

  static MsdVslDevice* cast(msd_device_t* dev) {
    DASSERT(dev);
    DASSERT(dev->magic_ == kMagic);
    return static_cast<MsdVslDevice*>(dev);
  }

 private:
  static constexpr uint32_t kRingbufferSizeInPages = 1;
  // Number of new commands added to the ringbuffer for each submitted batch:
  // EVENT, WAIT, LINK
  static constexpr uint32_t kRbInstructionsPerBatch = 3;
  // Number of new instructions added to the ringbuffer for flushing the TLB:
  // LOAD_STATE, SEMAPHORE, STALL, WAIT, LINK
  // This is in addition to |kRbInstructionsPerBatch|.
  static constexpr uint32_t kRbInstructionsPerFlush = 5;
  // Includes an additional instruction for address space switching.
  static constexpr uint32_t kRbMaxInstructionsPerEvent =
      kRbInstructionsPerBatch + kRbInstructionsPerFlush + 1;

  static constexpr uint32_t kInvalidRingbufferOffset = ~0;

  // The hardware provides 30 bits for interrupt events and 2 bits for errors.
  static constexpr uint32_t kNumEvents = 30;
  struct Event {
    bool allocated = false;
    bool submitted = false;
    bool free_on_complete = false;

    // The offset following this event in the ringbuffer.
    uint32_t ringbuffer_offset = kInvalidRingbufferOffset;
    std::unique_ptr<MappedBatch> mapped_batch;
    // If |mapped_batch| requires an address space switch, this will be populated with the
    // address space the ringbuffer was last configured with, to ensure it stays alive until the
    // switch is completed by hardware.
    std::shared_ptr<AddressSpace> prev_address_space;
  };

#define CHECK_THREAD_IS_CURRENT(x) \
  if (x)                           \
  DASSERT(magma::ThreadIdCheck::IsCurrent(*x))

#define CHECK_THREAD_NOT_CURRENT(x) \
  if (x)                            \
  DASSERT(!magma::ThreadIdCheck::IsCurrent(*x))

  bool Init(void* device_handle);
  bool HardwareInit();
  void Reset();
  void DisableInterrupts();

  // Appends the formatted string constructed from |fmt| and var args to |dump_out|.
  void OutputFormattedString(std::vector<std::string>* dump_out, const char* fmt, ...);
  // Populates |dump_out| with a formatted representation of |dump_state|.
  void FormatDump(DumpState* dump_state, std::vector<std::string>* dump_out);

  void StartDeviceThread();
  int DeviceThreadLoop();
  void EnqueueDeviceRequest(std::unique_ptr<DeviceRequest> request);

  int InterruptThreadLoop();
  magma::Status ProcessInterrupt();
  magma::Status ProcessDumpStatusToLog();
  void ProcessRequestBacklog();

  // Events for triggering interrupts.
  // If |free_on_complete| is true, the event will be freed automatically after the corresponding
  // interrupt is received.
  bool AllocInterruptEvent(bool free_on_complete, uint32_t* out_event_id);
  bool FreeInterruptEvent(uint32_t event_id);
  // Writes a new interrupt event to the end of the ringbuffer. The event must have been allocated
  // using |AllocInterruptEvent|.
  bool WriteInterruptEvent(uint32_t event_id, std::unique_ptr<MappedBatch> mapped_batch,
                           std::shared_ptr<AddressSpace> address_space);
  bool CompleteInterruptEvent(uint32_t event_id);

  bool MapRingbuffer(std::shared_ptr<MsdVslContext> context);

  // Returns true if starting the ringbuffer succeeded, or the ringbuffer was already running.
  bool StartRingbuffer(std::shared_ptr<MsdVslContext> context);
  // Adds a WAIT-LINK to the end of the ringbuffer.
  bool AddRingbufferWaitLink();
  // Modifies the last WAIT in the ringbuffer to link to |gpu_addr|.
  // |wait_link_offset| is the offset into the ringbuffer of the WAIT-LINK to replace.
  // |dest_prefetch| is the prefetch of the buffer we are linking to.
  bool LinkRingbuffer(uint32_t wait_link_offset, uint32_t gpu_addr, uint32_t dest_prefetch);

  // Writes a LINK command at the end of the given buffer.
  bool WriteLinkCommand(magma::PlatformBuffer* buf, uint32_t write_offset, uint32_t length,
                        uint16_t prefetch, uint32_t link_addr);

  // Returns whether the device became idle before |timeout_ms| elapsed.
  bool WaitUntilIdle(uint32_t timeout_ms);
  bool LoadInitialAddressSpace(std::shared_ptr<MsdVslContext> context,
                               uint32_t address_space_index);

  // If |prefetch_out| is not null, it will be populated with the prefetch that was submitted
  // to the device.
  bool SubmitCommandBufferNoMmu(uint64_t bus_addr, uint32_t length,
                                uint16_t* prefetch_out = nullptr);

  // If address space of |context| is not the same as |configured_address_space|,
  // the hardware will be configured with the new address space.
  bool SubmitFlushTlb(std::shared_ptr<MsdVslContext> context);

  bool SubmitCommandBuffer(std::shared_ptr<MsdVslContext> context, uint32_t address_space_index,
                           bool do_flush, std::unique_ptr<MappedBatch> mapped_batch,
                           uint32_t event_id);

  magma::Status ProcessBatch(std::unique_ptr<MappedBatch> batch, bool do_flush);

  magma::RegisterIo* register_io() { return register_io_.get(); }

  // AddressSpace::Owner
  magma::PlatformBusMapper* GetBusMapper() override { return bus_mapper_.get(); }

  void AddressSpaceReleased(AddressSpace* address_space) override {
    // Free is thread safe.
    page_table_slot_allocator_->Free(address_space->page_table_array_slot());
  }

  // MsdVslConnection::Owner
  Ringbuffer* GetRingbuffer() override { return ringbuffer_.get(); }

  // If |do_flush| is true, a flush TLB command will be queued before the batch commands.
  magma::Status SubmitBatch(std::unique_ptr<MappedBatch> batch, bool do_flush) override;

  PageTableArrays* page_table_arrays() { return page_table_arrays_.get(); }

  static const uint32_t kMagic = 0x64657669;  //"devi"

  std::unique_ptr<magma::PlatformDevice> platform_device_;
  std::unique_ptr<magma::RegisterIo> register_io_;
  std::unique_ptr<GpuFeatures> gpu_features_;
  uint32_t device_id_ = 0;
  uint32_t revision_ = 0;
  std::unique_ptr<magma::PlatformBusMapper> bus_mapper_;
  std::unique_ptr<PageTableArrays> page_table_arrays_;
  std::unique_ptr<PageTableSlotAllocator> page_table_slot_allocator_;

  // The command queue.
  std::unique_ptr<Ringbuffer> ringbuffer_;
  // This holds the address space that the hardware would be configured with at the current point
  // in the ringbuffer. If a client's address_space differs from |configured_address_space_|,
  // |SubmitFlushTlb| will write the commands for loading the client's address space and flushing
  // the TLB prior to linking to the new command buffer.
  std::shared_ptr<AddressSpace> configured_address_space_;

  std::thread interrupt_thread_;
  std::unique_ptr<magma::PlatformInterrupt> interrupt_;
  std::atomic_bool stop_interrupt_thread_{false};

  std::thread device_thread_;
  std::unique_ptr<magma::PlatformThreadId> device_thread_id_;
  std::atomic_bool stop_device_thread_{false};

  // Stores the largest seen sequence number in all completed events.
  uint64_t max_completed_sequence_number_ = 0;
  uint64_t next_sequence_number_ = 1;

  class BatchRequest;
  class DumpRequest;
  class InterruptRequest;
  class MappingReleaseRequest;

  // Thread-shared data members
  std::unique_ptr<magma::PlatformSemaphore> device_request_semaphore_;
  std::mutex device_request_mutex_;
  std::list<std::unique_ptr<DeviceRequest>> device_request_list_;

  struct DeferredRequest {
    std::unique_ptr<MappedBatch> batch;
    bool do_flush;
  };

  std::list<DeferredRequest> request_backlog_;

  Event events_[kNumEvents] = {};

  friend class TestMsdVslDevice;
  friend class TestCommandBuffer;
  friend class TestDeviceDump_DumpBasic_Test;
  friend class TestDeviceDump_DumpCommandBuffer_Test;
  friend class TestDeviceDump_DumpEventBatch_Test;
  friend class TestExec;
  friend class TestExec_Backlog_Test;
  friend class TestExec_BacklogWithInvalidBatch_Test;
  friend class TestExec_ReuseGpuAddress_Test;
  friend class TestExec_SubmitBatchWithOffset_Test;
  friend class TestExec_SubmitBatchesMultipleContexts_Test;
  friend class TestExec_SwitchAddressSpace_Test;
  friend class TestExec_SwitchMultipleAddressSpaces_Test;
  friend class TestEvents;
  friend class TestEvents_AllocAndFree_Test;
  friend class TestEvents_Submit_Test;
  friend class TestEvents_WriteSameEvent_Test;
  friend class TestEvents_WriteUnorderedEventIds_Test;
  friend class MsdVslDeviceTest_FetchEngineDma_Test;
  friend class MsdVslDeviceTest_LoadAddressSpace_Test;
  friend class MsdVslDeviceTest_RingbufferCanHoldMaxEvents_Test;
};

#endif  // MSD_VSL_DEVICE_H
