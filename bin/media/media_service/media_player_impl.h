// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <vector>

#include "apps/media/lib/flog/flog.h"
#include "apps/media/lib/timeline/timeline.h"
#include "apps/media/lib/timeline/timeline_function.h"
#include "apps/media/src/net/media_player_net_stub.h"
#include "apps/media/services/logs/media_player_channel.fidl.h"
#include "apps/media/services/media_service.fidl.h"
#include "apps/media/services/media_source.fidl.h"
#include "apps/media/services/media_transport.fidl.h"
#include "apps/media/services/seeking_reader.fidl.h"
#include "apps/media/services/timeline_controller.fidl.h"
#include "apps/media/src/media_service/media_service_impl.h"
#include "apps/media/src/util/callback_joiner.h"
#include "apps/media/src/util/fidl_publisher.h"
#include "apps/netconnector/lib/net_stub_responder.h"
#include "lib/fidl/cpp/bindings/binding.h"

namespace media {

// Fidl agent that renders streams from an origin specified by URL.
class MediaPlayerImpl : public MediaServiceImpl::Product<MediaPlayer>,
                        public MediaPlayer {
 public:
  static std::shared_ptr<MediaPlayerImpl> Create(
      fidl::InterfaceHandle<SeekingReader> reader,
      fidl::InterfaceHandle<MediaRenderer> audio_renderer_handle,
      fidl::InterfaceHandle<MediaRenderer> video_renderer_handle,
      fidl::InterfaceRequest<MediaPlayer> request,
      MediaServiceImpl* owner);

  ~MediaPlayerImpl() override;

  // MediaPlayer implementation.
  void GetStatus(uint64_t version_last_seen,
                 const GetStatusCallback& callback) override;

  void Play() override;

  void Pause() override;

  void Seek(int64_t position) override;

 private:
  static constexpr int64_t kMinimumLeadTime = Timeline::ns_from_ms(30);

  // Internal state.
  enum class State {
    kWaiting,  // Waiting for some work to complete.
    kFlushed,  // Paused with no data in the pipeline.
    kPrimed,   // Paused with data in the pipeline.
    kPlaying,  // Time is progressing.
  };

  MediaPlayerImpl(fidl::InterfaceHandle<SeekingReader> reader_handle,
                  fidl::InterfaceHandle<MediaRenderer> audio_renderer_handle,
                  fidl::InterfaceHandle<MediaRenderer> video_renderer_handle,
                  fidl::InterfaceRequest<MediaPlayer> request,
                  MediaServiceImpl* owner);

  // Creates sinks for enabled streams.
  void CreateSinks(fidl::Array<MediaTypePtr> stream_types,
                   fidl::InterfaceHandle<MediaRenderer> audio_renderer_handle,
                   fidl::InterfaceHandle<MediaRenderer> video_renderer_handle);

  // Prepares a stream.
  void PrepareStream(fidl::InterfaceHandle<MediaRenderer> renderer_handle,
                     size_t index,
                     const MediaTypePtr& input_media_type,
                     const std::function<void()>& callback);

  // Takes action based on current state.
  void Update();

  // Sets the timeline transform.
  void SetTimelineTransform(
      float rate,
      int64_t reference_time,
      const TimelineConsumer::SetTimelineTransformCallback callback);

  // Creates a TimelineTransform for the specified rate.
  TimelineTransformPtr CreateTimelineTransform(float rate,
                                               int64_t reference_time);

  // Handles a status update from the source. When called with the default
  // argument values, initiates source status updates.
  void HandleSourceStatusUpdates(uint64_t version = MediaSource::kInitialStatus,
                                 MediaSourceStatusPtr status = nullptr);

  // Handles a status update from the control point. When called with the
  // default argument values, initiates control point. status updates.
  void HandleTimelineControlPointStatusUpdates(
      uint64_t version = MediaTimelineControlPoint::kInitialStatus,
      MediaTimelineControlPointStatusPtr status = nullptr);

  MediaServicePtr media_service_;
  MediaSourcePtr source_;
  MediaTimelineControllerPtr timeline_controller_;
  MediaTimelineControlPointPtr timeline_control_point_;
  TimelineConsumerPtr timeline_consumer_;
  std::vector<MediaSinkPtr> sinks_;
  netconnector::NetStubResponder<MediaPlayer, MediaPlayerNetStub> responder_;

  // The state we're currently in.
  State state_ = State::kWaiting;

  // The state we're trying to transition to, either because the client has
  // called |Play| or |Pause| or because we've hit end-of-stream.
  State target_state_ = State::kFlushed;

  // Whether we're currently at end-of-stream.
  bool end_of_stream_ = false;

  // The position we want to seek to (because the client called Seek) or
  // kUnspecifiedTime, which indicates there's no desire to seek.
  int64_t target_position_ = kUnspecifiedTime;

  // The subject time to be used for SetTimelineTransform. The value is
  // kUnspecifiedTime if there's no need to seek or the position we want to
  // seek to if there is.
  int64_t transform_subject_time_ = kUnspecifiedTime;

  // A function that translates local time into presentation time in ns.
  TimelineFunction timeline_function_;

  MediaMetadataPtr metadata_;
  ProblemPtr problem_;
  FidlPublisher<GetStatusCallback> status_publisher_;

  FLOG_INSTANCE_CHANNEL(logs::MediaPlayerChannel, log_channel_);
};

}  // namespace media
