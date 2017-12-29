// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/cloud_sync/impl/page_sync_impl.h"

#include <memory>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "lib/fsl/socket/strings.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/functional/make_copyable.h"
#include "lib/fxl/macros.h"
#include "peridot/bin/ledger/cloud_sync/impl/constants.h"
#include "peridot/bin/ledger/cloud_sync/impl/testing/test_page_cloud.h"
#include "peridot/bin/ledger/cloud_sync/impl/testing/test_page_storage.h"
#include "peridot/bin/ledger/cloud_sync/public/sync_state_watcher.h"
#include "peridot/bin/ledger/encryption/fake/fake_encryption_service.h"
#include "peridot/bin/ledger/storage/public/page_storage.h"
#include "peridot/bin/ledger/storage/testing/commit_empty_impl.h"
#include "peridot/bin/ledger/storage/testing/page_storage_empty_impl.h"
#include "peridot/lib/backoff/backoff.h"
#include "peridot/lib/callback/capture.h"
#include "peridot/lib/gtest/test_with_message_loop.h"

namespace cloud_sync {
namespace {

// Dummy implementation of a backoff policy, which always returns zero backoff
// time..
class TestBackoff : public backoff::Backoff {
 public:
  explicit TestBackoff(int* get_next_count) : get_next_count_(get_next_count) {}
  ~TestBackoff() override {}

  fxl::TimeDelta GetNext() override {
    (*get_next_count_)++;
    return fxl::TimeDelta::FromMilliseconds(50);
  }

  void Reset() override {}

  int* get_next_count_;
};

class TestSyncStateWatcher : public SyncStateWatcher {
 public:
  TestSyncStateWatcher() {}
  ~TestSyncStateWatcher() override{};

  void Notify(SyncStateContainer sync_state) override {
    if (!states.empty() && sync_state == *states.rbegin()) {
      return;
    }
    states.push_back(sync_state);
  }

  std::vector<SyncStateContainer> states;
};

class PageSyncImplTest : public gtest::TestWithMessageLoop {
 public:
  PageSyncImplTest()
      : storage_(&message_loop_),
        encryption_service_(message_loop_.task_runner()),
        page_cloud_(page_cloud_ptr_.NewRequest()) {
    std::unique_ptr<TestSyncStateWatcher> watcher =
        std::make_unique<TestSyncStateWatcher>();
    state_watcher_ = watcher.get();
    page_sync_ = std::make_unique<PageSyncImpl>(
        message_loop_.task_runner(), &storage_, &encryption_service_,
        std::move(page_cloud_ptr_),
        std::make_unique<TestBackoff>(&download_backoff_get_next_calls_),
        std::make_unique<TestBackoff>(&upload_backoff_get_next_calls_),
        [this] {
          error_callback_calls_++;
          message_loop_.PostQuitTask();
        },
        std::move(watcher));
  }
  ~PageSyncImplTest() override {}

 protected:
  enum class UploadStatus {
    ENABLED,
    DISABLED,
  };
  void StartPageSync(UploadStatus status = UploadStatus::ENABLED) {
    if (status == UploadStatus::ENABLED) {
      page_sync_->EnableUpload();
    }
    page_sync_->Start();
  }

  TestPageStorage storage_;
  encryption::FakeEncryptionService encryption_service_;
  cloud_provider::PageCloudPtr page_cloud_ptr_;
  TestPageCloud page_cloud_;
  int download_backoff_get_next_calls_ = 0;
  int upload_backoff_get_next_calls_ = 0;
  TestSyncStateWatcher* state_watcher_;
  std::unique_ptr<PageSyncImpl> page_sync_;
  int error_callback_calls_ = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(PageSyncImplTest);
};

// Verifies that the backlog of commits to upload returned from
// GetUnsyncedCommits() is uploaded to PageCloudHandler.
TEST_F(PageSyncImplTest, UploadBacklog) {
  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");
  page_sync_->SetOnIdle(MakeQuitTask());
  StartPageSync();

  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ("id1", page_cloud_.received_commits[0].id);
  EXPECT_EQ("content1", encryption_service_.DecryptCommitSynchronous(
                            page_cloud_.received_commits[0].data));
  EXPECT_EQ("id2", page_cloud_.received_commits[1].id);
  EXPECT_EQ("content2", encryption_service_.DecryptCommitSynchronous(
                            page_cloud_.received_commits[1].data));
  EXPECT_EQ(2u, storage_.commits_marked_as_synced.size());
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id1"));
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id2"));

  ASSERT_EQ(7u, state_watcher_->states.size());
  EXPECT_EQ(DOWNLOAD_BACKLOG, state_watcher_->states[0].download);
  EXPECT_EQ(DOWNLOAD_BACKLOG, state_watcher_->states[1].download);
  EXPECT_EQ(DOWNLOAD_BACKLOG, state_watcher_->states[2].download);
  EXPECT_EQ(DOWNLOAD_SETTING_REMOTE_WATCHER,
            state_watcher_->states[3].download);
  EXPECT_EQ(DOWNLOAD_IDLE, state_watcher_->states[4].download);
  EXPECT_EQ(DOWNLOAD_IDLE, state_watcher_->states[5].download);
  EXPECT_EQ(DOWNLOAD_IDLE, state_watcher_->states[6].download);

  EXPECT_EQ(UPLOAD_STOPPED, state_watcher_->states[0].upload);
  EXPECT_EQ(UPLOAD_SETUP, state_watcher_->states[1].upload);
  EXPECT_EQ(UPLOAD_WAIT_REMOTE_DOWNLOAD, state_watcher_->states[2].upload);
  EXPECT_EQ(UPLOAD_WAIT_REMOTE_DOWNLOAD, state_watcher_->states[3].upload);
  EXPECT_EQ(UPLOAD_WAIT_REMOTE_DOWNLOAD, state_watcher_->states[4].upload);
  EXPECT_EQ(UPLOAD_IN_PROGRESS, state_watcher_->states[5].upload);
  EXPECT_EQ(UPLOAD_IDLE, state_watcher_->states[6].upload);
}

// Verifies that the backlog of commits to upload returned from
// GetUnsyncedCommits() is uploaded to PageCloudHandler.
TEST_F(PageSyncImplTest, PageWatcher) {
  TestSyncStateWatcher watcher;
  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");
  page_sync_->SetOnIdle(MakeQuitTask());
  page_sync_->SetSyncWatcher(&watcher);
  StartPageSync();

  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(8u, watcher.states.size());
  EXPECT_EQ(DOWNLOAD_STOPPED, watcher.states[0].download);
  EXPECT_EQ(DOWNLOAD_BACKLOG, watcher.states[1].download);
  EXPECT_EQ(DOWNLOAD_BACKLOG, watcher.states[2].download);
  EXPECT_EQ(DOWNLOAD_BACKLOG, watcher.states[3].download);
  EXPECT_EQ(DOWNLOAD_SETTING_REMOTE_WATCHER, watcher.states[4].download);
  EXPECT_EQ(DOWNLOAD_IDLE, watcher.states[5].download);
  EXPECT_EQ(DOWNLOAD_IDLE, watcher.states[6].download);
  EXPECT_EQ(DOWNLOAD_IDLE, watcher.states[7].download);

  EXPECT_EQ(UPLOAD_STOPPED, watcher.states[0].upload);
  EXPECT_EQ(UPLOAD_STOPPED, watcher.states[1].upload);
  EXPECT_EQ(UPLOAD_SETUP, watcher.states[2].upload);
  EXPECT_EQ(UPLOAD_WAIT_REMOTE_DOWNLOAD, watcher.states[3].upload);
  EXPECT_EQ(UPLOAD_WAIT_REMOTE_DOWNLOAD, watcher.states[4].upload);
  EXPECT_EQ(UPLOAD_WAIT_REMOTE_DOWNLOAD, watcher.states[5].upload);
  EXPECT_EQ(UPLOAD_IN_PROGRESS, watcher.states[6].upload);
  EXPECT_EQ(UPLOAD_IDLE, watcher.states[7].upload);
}

// Verifies that sync pauses uploading commits when it is downloading a commit.
TEST_F(PageSyncImplTest, NoUploadWhenDownloading) {
  storage_.should_delay_add_commit_confirmation = true;

  page_sync_->SetOnIdle(MakeQuitTask());
  StartPageSync();
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_TRUE(page_cloud_.set_watcher.is_bound());
  fidl::Array<cloud_provider::CommitPtr> commits;
  commits.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.set_watcher->OnNewCommits(std::move(commits),
                                        convert::ToArray("44"), [] {});
  EXPECT_TRUE(RunLoopUntil(
      [this] { return storage_.add_commits_from_sync_calls > 0u; }));

  storage_.watcher_->OnNewCommits(
      storage_.NewCommit("id2", "content2")->AsList(),
      storage::ChangeSource::LOCAL);

  EXPECT_TRUE(RunLoopUntil(
      [&] { return !storage_.delayed_add_commit_confirmations.empty(); }));
  EXPECT_TRUE(page_cloud_.received_commits.empty());

  storage_.delayed_add_commit_confirmations.front()();

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_FALSE(page_cloud_.received_commits.empty());
}

TEST_F(PageSyncImplTest, UploadExistingCommitsOnlyAfterBacklogDownload) {
  // Verify that two local commits are not uploaded when download is in
  // progress.
  storage_.NewCommit("local1", "content1");
  storage_.NewCommit("local2", "content2");

  page_cloud_.commits_to_return.push_back(
      MakeTestCommit(&encryption_service_, "remote3", "content3"));
  page_cloud_.commits_to_return.push_back(
      MakeTestCommit(&encryption_service_, "remote4", "content4"));
  page_cloud_.position_token_to_return = convert::ToArray("43");
  bool backlog_downloaded_called = false;
  page_sync_->SetOnBacklogDownloaded([this, &backlog_downloaded_called] {
    EXPECT_EQ(0u, page_cloud_.received_commits.size());
    EXPECT_EQ(0u, storage_.commits_marked_as_synced.size());
    backlog_downloaded_called = true;
  });
  page_sync_->SetOnIdle(MakeQuitTask());
  StartPageSync();

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_TRUE(backlog_downloaded_called);
  ASSERT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ("local1", page_cloud_.received_commits[0].id);
  EXPECT_EQ("content1", encryption_service_.DecryptCommitSynchronous(
                            page_cloud_.received_commits[0].data));
  EXPECT_EQ("local2", page_cloud_.received_commits[1].id);
  EXPECT_EQ("content2", encryption_service_.DecryptCommitSynchronous(
                            page_cloud_.received_commits[1].data));
  ASSERT_EQ(2u, storage_.commits_marked_as_synced.size());
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("local1"));
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("local2"));
}

// Verifies that existing commits are uploaded before the new ones.
TEST_F(PageSyncImplTest, UploadExistingAndNewCommits) {
  storage_.NewCommit("id1", "content1");

  page_sync_->SetOnBacklogDownloaded([this] {
    message_loop_.task_runner()->PostTask([this] {
      auto commit = storage_.NewCommit("id2", "content2");
      storage_.new_commits_to_return["id2"] = commit->Clone();
      storage_.watcher_->OnNewCommits(commit->AsList(),
                                      storage::ChangeSource::LOCAL);
    });
  });
  page_sync_->SetOnIdle(MakeQuitTask());

  StartPageSync();
  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ("id1", page_cloud_.received_commits[0].id);
  EXPECT_EQ("content1", encryption_service_.DecryptCommitSynchronous(
                            page_cloud_.received_commits[0].data));
  EXPECT_EQ("id2", page_cloud_.received_commits[1].id);
  EXPECT_EQ("content2", encryption_service_.DecryptCommitSynchronous(
                            page_cloud_.received_commits[1].data));
  EXPECT_EQ(2u, storage_.commits_marked_as_synced.size());
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id1"));
  EXPECT_EQ(1u, storage_.commits_marked_as_synced.count("id2"));
}

// Verifies that the on idle callback is called when there is no pending upload
// tasks.
TEST_F(PageSyncImplTest, UploadIdleCallback) {
  int on_idle_calls = 0;

  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");

  page_sync_->SetOnIdle([this, &on_idle_calls] {
    on_idle_calls++;
    message_loop_.PostQuitTask();
  });
  StartPageSync();

  // Verify that the idle callback is called once both commits are uploaded.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(2u, page_cloud_.received_commits.size());
  EXPECT_EQ(1, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());

  // Notify about a new commit to upload and verify that the idle callback was
  // called again on completion.
  auto commit3 = storage_.NewCommit("id3", "content3");
  storage_.new_commits_to_return["id3"] = commit3->Clone();
  storage_.watcher_->OnNewCommits(commit3->AsList(),
                                  storage::ChangeSource::LOCAL);
  EXPECT_FALSE(page_sync_->IsIdle());
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(3u, page_cloud_.received_commits.size());
  EXPECT_EQ(2, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
}

// Verifies that a failure to persist the remote commit stops syncing remote
// commits and calls the error callback.
TEST_F(PageSyncImplTest, FailToStoreRemoteCommit) {
  page_sync_->SetOnIdle(MakeQuitTask());
  StartPageSync();
  EXPECT_FALSE(RunLoopWithTimeout());
  ASSERT_TRUE(page_cloud_.set_watcher.is_bound());

  fidl::Array<cloud_provider::CommitPtr> commits;
  commits.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  storage_.should_fail_add_commit_from_sync = true;
  EXPECT_EQ(0, error_callback_calls_);
  page_cloud_.set_watcher->OnNewCommits(std::move(commits),
                                        convert::ToArray("42"), [] {});

  EXPECT_TRUE(RunLoopUntil(
      [this] { return page_cloud_.set_watcher.encountered_error(); }));
  EXPECT_TRUE(RunLoopUntil([this] { return error_callback_calls_ == 1; }));
}

// Verifies that the on idle callback is called when there is no download in
// progress.
TEST_F(PageSyncImplTest, DownloadIdleCallback) {
  page_cloud_.commits_to_return.push_back(
      MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.commits_to_return.push_back(
      MakeTestCommit(&encryption_service_, "id2", "content2"));
  page_cloud_.position_token_to_return = convert::ToArray("43");

  int on_idle_calls = 0;
  page_sync_->SetOnIdle([this, &on_idle_calls] {
    on_idle_calls++;
    message_loop_.PostQuitTask();
  });
  StartPageSync();
  EXPECT_EQ(0, on_idle_calls);
  EXPECT_FALSE(page_sync_->IsIdle());

  // Run the message loop and verify that the sync is idle after all remote
  // commits are added to storage.
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
  EXPECT_EQ(2u, storage_.received_commits.size());

  // Notify about a new commit to download and verify that the idle callback was
  // called again on completion.
  fidl::Array<cloud_provider::CommitPtr> commits;
  commits.push_back(MakeTestCommit(&encryption_service_, "id3", "content3"));
  page_cloud_.set_watcher->OnNewCommits(std::move(commits),
                                        convert::ToArray("44"), [] {});
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(3u, storage_.received_commits.size());
  EXPECT_EQ(2, on_idle_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
}

// Verifies that uploads are paused until EnableUpload is called.
TEST_F(PageSyncImplTest, UploadIsPaused) {
  storage_.NewCommit("id1", "content1");
  storage_.NewCommit("id2", "content2");
  page_sync_->SetOnIdle(MakeQuitTask());

  StartPageSync(UploadStatus::DISABLED);
  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(0u, page_cloud_.received_commits.size());

  page_sync_->EnableUpload();
  EXPECT_FALSE(RunLoopWithTimeout());

  ASSERT_EQ(2u, page_cloud_.received_commits.size());
}

// Merge commits are deterministic, so can already be in the cloud when we try
// to upload it. The upload will then fail. However, we should stop retrying to
// upload the commit once we received a notification for it through the cloud
// sync watcher.
TEST_F(PageSyncImplTest, UploadCommitAlreadyInCloud) {
  // Complete the initial sync.
  StartPageSync();
  EXPECT_TRUE(
      RunLoopUntil([this] { return page_cloud_.get_commits_calls > 0u; }));

  // Create a local commit, but make the upload fail.
  page_cloud_.commit_status_to_return = cloud_provider::Status::SERVER_ERROR;
  auto commit1 = storage_.NewCommit("id1", "content1");
  storage_.new_commits_to_return["id1"] = commit1->Clone();
  storage_.watcher_->OnNewCommits(commit1->AsList(),
                                  storage::ChangeSource::LOCAL);

  EXPECT_TRUE(RunLoopUntil([this] {
    return page_cloud_.add_commits_calls == 1u &&
           // We need to wait for the callback to be executed on the PageSync
           // side.
           upload_backoff_get_next_calls_ == 1;
  }));

  // Verify that the commit is still not marked as synced in storage.
  EXPECT_TRUE(storage_.commits_marked_as_synced.empty());
  EXPECT_EQ(1, upload_backoff_get_next_calls_);

  // Let's receive the same commit from the remote side.
  fidl::Array<cloud_provider::CommitPtr> commits;
  commits.push_back(MakeTestCommit(&encryption_service_, "id1", "content1"));
  page_cloud_.set_watcher->OnNewCommits(std::move(commits),
                                        convert::ToArray("44"), [] {});
  EXPECT_TRUE(RunLoopUntil([this] { return page_sync_->IsIdle(); }));

  // No additional calls.
  EXPECT_EQ(1u, page_cloud_.add_commits_calls);
  EXPECT_TRUE(page_sync_->IsIdle());
}

}  // namespace
}  // namespace cloud_sync
