// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/zx/handle.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zircon/compiler.h>

#include <iterator>

#include <fbl/algorithm.h>
#include <fbl/unique_fd.h>

#include "filesystems.h"
#include "misc.h"

namespace fio = ::llcpp::fuchsia::io;

bool TestRenameBasic() {
  BEGIN_TEST;
  // Cannot rename when src does not exist
  ASSERT_EQ(rename("::alpha", "::bravo"), -1, "");

  // Renaming to self is fine
  ASSERT_EQ(mkdir("::alpha", 0755), 0, "");
  ASSERT_EQ(rename("::alpha", "::alpha"), 0, "");
  ASSERT_EQ(rename("::alpha/.", "::alpha/."), 0, "");
  ASSERT_EQ(rename("::alpha/", "::alpha"), 0, "");
  ASSERT_EQ(rename("::alpha", "::alpha/"), 0, "");
  ASSERT_EQ(rename("::alpha/", "::alpha/"), 0, "");
  ASSERT_EQ(rename("::alpha/./../alpha", "::alpha/./../alpha"), 0, "");

  // Cannot rename dir to file
  int fd = open("::bravo", O_RDWR | O_CREAT | O_EXCL, 0644);
  ASSERT_GT(fd, 0, "");
  ASSERT_EQ(close(fd), 0, "");
  ASSERT_EQ(rename("::alpha", "::bravo"), -1, "");
  ASSERT_EQ(unlink("::bravo"), 0, "");

  // Rename dir (dst does not exist)
  ASSERT_EQ(rename("::alpha", "::bravo"), 0, "");
  ASSERT_EQ(mkdir("::alpha", 0755), 0, "");
  // Rename dir (dst does exist)
  ASSERT_EQ(rename("::bravo", "::alpha"), 0, "");

  // Rename file (dst does not exist)
  fd = open("::alpha/charlie", O_RDWR | O_CREAT | O_EXCL, 0644);
  ASSERT_GT(fd, 0, "");
  ASSERT_EQ(rename("::alpha/charlie", "::alpha/delta"), 0, "");
  // File rename to self
  ASSERT_EQ(rename("::alpha/delta", "::alpha/delta"), 0, "");
  // Not permitted with trailing '/'
  ASSERT_EQ(rename("::alpha/delta", "::alpha/delta/"), -1, "");
  ASSERT_EQ(rename("::alpha/delta/", "::alpha/delta"), -1, "");
  ASSERT_EQ(rename("::alpha/delta/", "::alpha/delta/"), -1, "");
  ASSERT_EQ(close(fd), 0, "");

  // Rename file (dst does not exist)
  fd = open("::alpha/charlie", O_RDWR | O_CREAT | O_EXCL, 0644);
  ASSERT_GT(fd, 0, "");
  ASSERT_EQ(rename("::alpha/delta", "::alpha/charlie"), 0, "");
  ASSERT_EQ(close(fd), 0, "");

  // Rename to different directory
  ASSERT_EQ(mkdir("::bravo", 0755), 0, "");
  ASSERT_EQ(rename("::alpha/charlie", "::charlie"), 0, "");
  ASSERT_EQ(rename("::charlie", "::alpha/charlie"), 0, "");
  ASSERT_EQ(rename("::bravo", "::alpha/bravo"), 0, "");
  ASSERT_EQ(rename("::alpha/charlie", "::alpha/bravo/charlie"), 0, "");

  // Cannot rename directory to subdirectory of itself
  ASSERT_EQ(rename("::alpha", "::alpha/bravo"), -1, "");
  ASSERT_EQ(rename("::alpha", "::alpha/bravo/charlie"), -1, "");
  ASSERT_EQ(rename("::alpha", "::alpha/bravo/charlie/delta"), -1, "");
  ASSERT_EQ(rename("::alpha", "::alpha/delta"), -1, "");
  ASSERT_EQ(rename("::alpha/bravo", "::alpha/bravo/charlie"), -1, "");
  ASSERT_EQ(rename("::alpha/bravo", "::alpha/bravo/charlie/delta"), -1, "");
  // Cannot rename to non-empty directory
  ASSERT_EQ(rename("::alpha/bravo/charlie", "::alpha/bravo"), -1, "");
  ASSERT_EQ(rename("::alpha/bravo/charlie", "::alpha"), -1, "");
  ASSERT_EQ(rename("::alpha/bravo", "::alpha"), -1, "");

  // Clean up
  ASSERT_EQ(unlink("::alpha/bravo/charlie"), 0, "");
  ASSERT_EQ(unlink("::alpha/bravo"), 0, "");
  ASSERT_EQ(unlink("::alpha"), 0, "");

  END_TEST;
}

bool TestRenameWithChildren() {
  BEGIN_TEST;

  ASSERT_EQ(mkdir("::dir_before_move", 0755), 0, "");
  ASSERT_EQ(mkdir("::dir_before_move/dir1", 0755), 0, "");
  ASSERT_EQ(mkdir("::dir_before_move/dir2", 0755), 0, "");
  ASSERT_EQ(mkdir("::dir_before_move/dir2/subdir", 0755), 0, "");
  int fd = open("::dir_before_move/file", O_RDWR | O_CREAT, 0644);
  ASSERT_GT(fd, 0, "");

  const char file_contents[] = "This should be in the file";
  ASSERT_STREAM_ALL(write, fd, (uint8_t*)file_contents, strlen(file_contents));

  ASSERT_EQ(rename("::dir_before_move", "::dir"), 0, "Could not rename");

  // Check that the directory layout has persisted across rename
  expected_dirent_t dir_contents[] = {
      {false, ".", DT_DIR},
      {false, "dir1", DT_DIR},
      {false, "dir2", DT_DIR},
      {false, "file", DT_REG},
  };
  ASSERT_TRUE(check_dir_contents("::dir", dir_contents, std::size(dir_contents)), "");
  expected_dirent_t dir2_contents[] = {
      {false, ".", DT_DIR},
      {false, "subdir", DT_DIR},
  };
  ASSERT_TRUE(check_dir_contents("::dir/dir2", dir2_contents, std::size(dir2_contents)), "");

  // Check the our file data has lasted (without re-opening)
  ASSERT_TRUE(check_file_contents(fd, (uint8_t*)file_contents, strlen(file_contents)), "");

  // Check the our file data has lasted (with re-opening)
  ASSERT_EQ(close(fd), 0, "");
  fd = open("::dir/file", O_RDONLY, 06444);
  ASSERT_GT(fd, 0, "");
  ASSERT_TRUE(check_file_contents(fd, (uint8_t*)file_contents, strlen(file_contents)), "");
  ASSERT_EQ(close(fd), 0, "");

  // Clean up
  ASSERT_EQ(unlink("::dir/dir1"), 0, "");
  ASSERT_EQ(unlink("::dir/dir2/subdir"), 0, "");
  ASSERT_EQ(unlink("::dir/dir2"), 0, "");
  ASSERT_EQ(unlink("::dir/file"), 0, "");
  ASSERT_EQ(unlink("::dir"), 0, "");

  END_TEST;
}

bool TestRenameAbsoluteRelative() {
  BEGIN_TEST;

  char cwd[PATH_MAX];
  ASSERT_NONNULL(getcwd(cwd, sizeof(cwd)), "");

  // Change the cwd to a known directory
  ASSERT_EQ(mkdir("::working_dir", 0755), 0, "");
  DIR* dir = opendir("::working_dir");
  ASSERT_NONNULL(dir, "");
  ASSERT_EQ(chdir("::working_dir"), 0, "");

  // Make a "foo" directory in the cwd
  int fd = dirfd(dir);
  ASSERT_NE(fd, -1, "");
  ASSERT_EQ(mkdirat(fd, "foo", 0755), 0, "");
  expected_dirent_t dir_contents_foo[] = {
      {false, ".", DT_DIR},
      {false, "foo", DT_DIR},
  };
  ASSERT_TRUE(fcheck_dir_contents(dir, dir_contents_foo, std::size(dir_contents_foo)), "");

  // Rename "foo" to "bar" using mixed paths
  ASSERT_EQ(rename("::working_dir/foo", "bar"), 0, "Could not rename foo to bar");
  expected_dirent_t dir_contents_bar[] = {
      {false, ".", DT_DIR},
      {false, "bar", DT_DIR},
  };
  ASSERT_TRUE(fcheck_dir_contents(dir, dir_contents_bar, std::size(dir_contents_bar)), "");

  // Rename "bar" back to "foo" using mixed paths in the other direction
  ASSERT_EQ(rename("bar", "::working_dir/foo"), 0, "Could not rename bar to foo");
  ASSERT_TRUE(fcheck_dir_contents(dir, dir_contents_foo, std::size(dir_contents_foo)), "");

  ASSERT_EQ(rmdir("::working_dir/foo"), 0, "");

  // Change the cwd back to the original, whatever it was before
  // this test started
  ASSERT_EQ(chdir(cwd), 0, "Could not return to original cwd");

  ASSERT_EQ(rmdir("::working_dir"), 0, "");
  ASSERT_EQ(closedir(dir), 0, "");

  END_TEST;
}

bool TestRenameAt() {
  BEGIN_TEST;

  ASSERT_EQ(mkdir("::foo", 0755), 0, "");
  ASSERT_EQ(mkdir("::foo/baz", 0755), 0, "");
  ASSERT_EQ(mkdir("::bar", 0755), 0, "");

  // Normal case of renameat, from one directory to another
  int foofd = open("::foo", O_RDONLY | O_DIRECTORY, 0644);
  ASSERT_GT(foofd, 0, "");
  int barfd = open("::bar", O_RDONLY | O_DIRECTORY, 0644);
  ASSERT_GT(barfd, 0, "");

  ASSERT_EQ(renameat(foofd, "baz", barfd, "zab"), 0, "");

  expected_dirent_t empty_contents[] = {
      {false, ".", DT_DIR},
  };
  ASSERT_TRUE(check_dir_contents("::foo", empty_contents, std::size(empty_contents)), "");
  expected_dirent_t contains_zab[] = {
      {false, ".", DT_DIR},
      {false, "zab", DT_DIR},
  };
  ASSERT_TRUE(check_dir_contents("::bar", contains_zab, std::size(contains_zab)), "");

  // Alternate case of renameat, where an absolute path ignores
  // the file descriptor.
  //
  // Here, barfd is used (in the first argument) but ignored (in the second argument).
  ASSERT_EQ(renameat(barfd, "zab", barfd, "::foo/baz"), 0, "");
  expected_dirent_t contains_baz[] = {
      {false, ".", DT_DIR},
      {false, "baz", DT_DIR},
  };
  ASSERT_TRUE(check_dir_contents("::foo", contains_baz, std::size(contains_baz)), "");
  ASSERT_TRUE(check_dir_contents("::bar", empty_contents, std::size(empty_contents)), "");

  // The 'absolute-path-ignores-fd' case should also work with invalid fds.
  ASSERT_EQ(renameat(-1, "::foo/baz", -1, "::bar/baz"), 0, "");
  ASSERT_TRUE(check_dir_contents("::foo", empty_contents, std::size(empty_contents)), "");
  ASSERT_TRUE(check_dir_contents("::bar", contains_baz, std::size(contains_baz)), "");

  // However, relative paths should not be allowed with invalid fds.
  ASSERT_EQ(renameat(-1, "baz", foofd, "baz"), -1, "");
  ASSERT_EQ(errno, EBADF, "");

  // Additionally, we shouldn't be able to renameat to a file.
  int fd = openat(barfd, "filename", O_CREAT | O_RDWR | O_EXCL);
  ASSERT_GT(fd, 0, "");
  ASSERT_EQ(renameat(foofd, "baz", fd, "baz"), -1, "");
  // NOTE: not checking for "ENOTDIR", since ENOTSUPPORTED might be returned instead.

  // Clean up
  ASSERT_EQ(close(fd), 0, "");
  ASSERT_EQ(unlink("::bar/filename"), 0, "");
  ASSERT_EQ(rmdir("::bar/baz"), 0, "");
  ASSERT_EQ(close(foofd), 0, "");
  ASSERT_EQ(close(barfd), 0, "");
  ASSERT_EQ(rmdir("::foo"), 0, "");
  ASSERT_EQ(rmdir("::bar"), 0, "");
  END_TEST;
}

// Rename using the raw FIDL interface.
bool TestRenameRaw() {
  BEGIN_TEST;

  ASSERT_EQ(mkdir("::alpha", 0755), 0, "");
  ASSERT_EQ(mkdir("::alpha/bravo", 0755), 0, "");
  ASSERT_EQ(mkdir("::alpha/bravo/charlie", 0755), 0, "");

  fbl::unique_fd fd(open("::alpha", O_RDONLY | O_DIRECTORY, 0644));
  ASSERT_TRUE(fd);
  fdio_cpp::FdioCaller caller(std::move(fd));

  auto token_result = fio::Directory::Call::GetToken(caller.channel());
  ASSERT_EQ(token_result.status(), ZX_OK);
  ASSERT_EQ(token_result.Unwrap()->s, ZX_OK);

  // Pass a path, instead of a name, to rename.
  // Observe that paths are rejected.
  constexpr char src[] = "bravo/charlie";
  constexpr char dst[] = "bravo/delta";
  auto rename_result =
      fio::Directory::Call::Rename(caller.channel(), fidl::StringView(src),
                                   std::move(token_result.Unwrap()->token), fidl::StringView(dst));
  ASSERT_EQ(rename_result.status(), ZX_OK);
  ASSERT_EQ(rename_result.Unwrap()->s, ZX_ERR_INVALID_ARGS);

  // Clean up
  ASSERT_EQ(unlink("::alpha/bravo/charlie"), 0, "");
  ASSERT_EQ(unlink("::alpha/bravo"), 0, "");
  ASSERT_EQ(unlink("::alpha"), 0, "");

  END_TEST;
}

RUN_FOR_ALL_FILESYSTEMS(rename_tests,
                        RUN_TEST_MEDIUM(TestRenameBasic) RUN_TEST_MEDIUM(TestRenameWithChildren)
                            RUN_TEST_MEDIUM(TestRenameAbsoluteRelative)
                                RUN_TEST_MEDIUM(TestRenameAt) RUN_TEST_MEDIUM(TestRenameRaw))
