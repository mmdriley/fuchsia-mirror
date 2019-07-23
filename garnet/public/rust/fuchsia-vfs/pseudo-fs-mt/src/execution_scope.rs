// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Values of this type represent "execution scopes" used by the library to give fine grained
//! control of the lifetimes of the tasks associated with particular connections.  When a new
//! connection is attached to a pseudo directory tree, an execution scope is provided.  This scope
//! is then used to start any tasks related to this connection.  All connections opened as a result
//! of operations on this first connection will also use the same scope, as well as any tasks
//! related to those connections.
//!
//! This way, it is possible to control the lifetime of a group of connections.  All connections
//! and their tasks can be shutdown by calling `shutdown` method on the scope that is hosting them.
//! Scope will also shutdown all the tasks when it goes out of scope.
//!
//! Implementation wise, execution scope is just a proxy, that forwards all the tasks to an actual
//! executor, provided as an instance of a [`Spawn`] trait.

use {
    futures::{
        channel::oneshot,
        select,
        task::{self, Context, Poll, Spawn},
        Future, FutureExt,
    },
    parking_lot::Mutex,
    pin_utils::unsafe_pinned,
    slab::Slab,
    std::{ops::Drop, pin::Pin, sync::Arc},
};

pub type SpawnError = task::SpawnError;

/// An execution scope that is hosting tasks for a group of connections.  See the module level
/// documentation for details.
///
/// Actual execution will be delegated to an "upstream" executor - something that implements
/// [`Spawn`].  In a sense, this is somewhat of an analog of a multithreaded capable
/// [`FuturesUnordered`], but this some additional functionality specific to the pseudo-fs-mt
/// library.
pub struct ExecutionScope {
    inner: Arc<Mutex<Inner>>,
}

struct Inner {
    upstream: Box<dyn Spawn + Send>,
    /// This is a list of shutdown channels for all the tasks that might be currently running.
    /// When we initiate a task shutdown by sending a message over the channel, but as we need to
    /// consume the sender in the process, we use `Option`s turning the consumed ones into `None`s.
    running: Slab<Option<oneshot::Sender<()>>>,
}

impl ExecutionScope {
    /// Constructs a new execution scope, wrapping the specified executor.
    pub fn new(upstream: Box<dyn Spawn + Send>) -> Self {
        let inner = Arc::new(Mutex::new(Inner { upstream, running: Slab::new() }));
        ExecutionScope { inner }
    }

    /// Sends a `task` to be executed in this execution scope.  This is very similar to
    /// [`Spawn::spawn_obj`] with a minor difference that `self` reference is not exclusive.
    ///
    /// Note that when the scope is shut down, this task will be interrupted the next time it
    /// returns `Pending`.  If you need to perform any shutdown operations, use
    /// [`spawn_with_shutdown`] instead.
    ///
    /// For the "pseudo-fs-mt" library it is more convenient that this method allows non-exclusive
    /// access.  And as the implementation is employing internal mutability there are no downsides.
    /// This way `ExecutionScope` can actually also implement [`Spawn`] - it just was not necessary
    /// for now.
    pub fn spawn<Task>(&self, task: Task) -> Result<(), SpawnError>
    where
        Task: Future<Output = ()> + Send + 'static,
    {
        Inner::run_abort_any_time(self.inner.clone(), Box::pin(task))
    }

    /// Sends a `task` to be executed in this execution scope.  This is very similar to
    /// [`Spawn::spawn_obj`] with a minor difference that `self` reference is not exclusive.
    ///
    /// Task to be executed will be constructed using the specified callback.  It is provided with
    /// a one-shot channel that will be signaled during the shutdown process.  The task must be
    /// monitoring the channel and should perform any necessary shutdown steps and terminate when
    /// a message is received over the channel.  If you do not need a custom shutdown process you
    /// can use [`spawn`] method instead.
    ///
    /// For the "pseudo-fs-mt" library it is more convenient that this method allows non-exclusive
    /// access.  And as the implementation is employing internal mutability there are no downsides.
    /// This way `ExecutionScope` can actually also implement [`Spawn`] - it just was not necessary
    /// for now.
    pub fn spawn_with_shutdown<Constructor, Task>(
        &self,
        constructor: Constructor,
    ) -> Result<(), SpawnError>
    where
        Constructor: FnOnce(oneshot::Receiver<()>) -> Task + 'static,
        Task: Future<Output = ()> + Send + 'static,
    {
        Inner::run_abort_with_shutdown(
            self.inner.clone(),
            Box::new(|shutdown| Box::pin(constructor(shutdown))),
        )
    }

    pub fn shutdown(&self) {
        let mut this = self.inner.lock();
        this.shutdown();
    }
}

impl Clone for ExecutionScope {
    fn clone(&self) -> Self {
        ExecutionScope { inner: self.inner.clone() }
    }
}

type FutureConstructorWithArg<ArgT> =
    Box<dyn FnOnce(ArgT) -> Pin<Box<dyn Future<Output = ()> + Send>>>;

struct RemoveFromRunning<Wrapped>
where
    Wrapped: Future<Output = ()> + Send,
{
    id: usize,
    inner: Arc<Mutex<Inner>>,
    task: Wrapped,
}

impl<Wrapped> RemoveFromRunning<Wrapped>
where
    Wrapped: Future<Output = ()> + Send,
{
    // unsafe: `RemoveFromRunning::drop` does not move the `task` value.  `RemoveFromRunning` also
    // does not implement `Unpin`.  `task` is not `#[repr(packed)]`.
    unsafe_pinned!(task: Wrapped);

    fn new(id: usize, inner: Arc<Mutex<Inner>>, task: Wrapped) -> Self {
        RemoveFromRunning { id, inner, task }
    }
}

impl<Wrapped> Future for RemoveFromRunning<Wrapped>
where
    Wrapped: Future<Output = ()> + Send,
{
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
        self.as_mut().task().poll_unpin(cx)
    }
}

impl<Wrapped> Drop for RemoveFromRunning<Wrapped>
where
    Wrapped: Future<Output = ()> + Send,
{
    fn drop(&mut self) {
        let mut this = self.inner.lock();
        this.running.remove(self.id);
    }
}

impl Inner {
    fn run_abort_any_time(
        inner: Arc<Mutex<Inner>>,
        task: Pin<Box<dyn Future<Output = ()> + Send>>,
    ) -> Result<(), SpawnError> {
        Self::run_abort_with_shutdown(
            inner,
            Box::new(move |stop_receiver| {
                // We need a future that would complete when either `task` or `stop_receiver`
                // complete.  If there would be a combinator, similar to `join` that would do that,
                // this whole block is unnecessary.  But I could not find anything sutable in
                // `futures-rs`.
                Box::pin(async move {
                    let mut task = task.fuse();
                    let mut stop_receiver = stop_receiver.fuse();
                    loop {
                        select! {
                            () = task => {
                                break;
                            },
                            _ = stop_receiver => {
                                break;
                            },
                        };
                    }
                })
            }),
        )
    }

    fn run_abort_with_shutdown(
        inner: Arc<Mutex<Inner>>,
        constructor: FutureConstructorWithArg<oneshot::Receiver<()>>,
    ) -> Result<(), SpawnError> {
        let mut this = inner.lock();

        let (stop_sender, stop_receiver) = oneshot::channel();
        let task_id = this.running.insert(Some(stop_sender));
        let task = constructor(stop_receiver);
        let task = RemoveFromRunning::new(task_id, inner.clone(), task);

        match this.upstream.spawn_obj(Box::pin(task).into()) {
            Ok(()) => Ok(()),
            Err(err) => {
                this.running.remove(task_id);
                Err(err)
            }
        }
    }

    fn shutdown(&mut self) {
        for (_key, task) in self.running.iter_mut() {
            let sender = match task.take() {
                None => {
                    // As the task removal is processed by they task itself, we may see cases when
                    // we have already sent the stop message, but the task did not remove it'se
                    // entry from the list just yet.  There is a race condition with the task
                    // shutdown process.  Shutdown happens in one thread, while task execution - in
                    // another.  So, we need to tolerate "double" removal either here, or in the
                    // task shutdown code.  Making the task shutodwn code responsible from removing
                    // itself from the `running` list seems a bit cleaner.
                    continue;
                }
                Some(sender) => sender,
            };

            if sender.send(()).is_ok() {
                continue;
            }

            // Receiver should only be destroyed after the sender has been removed from the running
            // tasks list.  So, an `Err` here is a logical error.  But crashing here is also not
            // very helpful, except in a controlled environment, so there seems to be little reason
            // to assert in non-debug mode.
            debug_assert!(false, "Execution scope has an entry that it can not communicate with.");
        }
    }
}

impl Drop for Inner {
    fn drop(&mut self) {
        self.shutdown();
    }
}

#[cfg(test)]
mod tests {
    use super::ExecutionScope;

    use {
        fuchsia_async::Executor,
        futures::{
            channel::{mpsc, oneshot},
            select,
            task::Poll,
            Future, FutureExt, StreamExt,
        },
        pin_utils::pin_mut,
        std::sync::{
            atomic::{AtomicUsize, Ordering},
            Arc,
        },
    };

    fn run_test<GetTest, GetTestRes>(get_test: GetTest)
    where
        GetTest: FnOnce(ExecutionScope) -> GetTestRes,
        GetTestRes: Future<Output = ()>,
    {
        let mut exec = Executor::new().expect("Executor creation failed");

        let scope = ExecutionScope::new(Box::new(exec.ehandle()));

        let test = get_test(scope);

        pin_mut!(test);
        assert_eq!(exec.run_until_stalled(&mut test), Poll::Ready(()), "Test did not complete");
    }

    #[test]
    fn simple() {
        run_test(|scope| async move {
            let (sender, receiver) = oneshot::channel();
            let (counters, task) = mocks::ImmediateTask::new(sender);

            scope.spawn(task).unwrap();

            // Make sure our task hand a change to execute.
            await!(receiver).unwrap();

            assert_eq!(counters.drop_call(), 1);
            assert_eq!(counters.poll_call(), 1);
        });
    }

    #[test]
    fn simple_drop() {
        run_test(|scope| async move {
            let (poll_sender, poll_receiver) = oneshot::channel();
            let (processing_done_sender, processing_done_receiver) = oneshot::channel();
            let (drop_sender, drop_receiver) = oneshot::channel();
            let (counters, task) =
                mocks::ControlledTask::new(poll_sender, processing_done_receiver, drop_sender);

            scope.spawn(task).unwrap();

            await!(poll_receiver).unwrap();

            processing_done_sender.send(()).unwrap();

            scope.shutdown();

            await!(drop_receiver).unwrap();

            // poll might be called one or two times, and it seems to be called differnet number of
            // times depending on the run...  Not sure why is this happaning.  As this test is
            // single threaded and just async I would imagine the execution to be deterministic.
            let poll_count = counters.poll_call();
            assert!(poll_count >= 1, "poll was not called");

            assert_eq!(counters.drop_call(), 1);
        });
    }

    #[test]
    fn spawn_with_shutdown() {
        run_test(|scope| async move {
            let (processing_done_sender, processing_done_receiver) = oneshot::channel();
            let (shutdown_complete_sender, shutdown_complete_receiver) = oneshot::channel();

            scope
                .spawn_with_shutdown(|_shutdown| async move {
                    await!(processing_done_receiver).unwrap();
                    shutdown_complete_sender.send(()).unwrap();
                })
                .unwrap();

            processing_done_sender.send(()).unwrap();

            await!(shutdown_complete_receiver).unwrap();
        });
    }

    #[test]
    fn explicit_shutdown() {
        run_test(|scope| async move {
            let (tick_sender, tick_receiver) = mpsc::unbounded();
            let (tick_confirmation_sender, mut tick_confirmation_receiver) = mpsc::unbounded();
            let (shutdown_complete_sender, shutdown_complete_receiver) = oneshot::channel();

            let tick_count = Arc::new(AtomicUsize::new(0));

            scope
                .spawn_with_shutdown({
                    let tick_count = tick_count.clone();

                    |shutdown| async move {
                        let mut tick_receiver = tick_receiver.fuse();
                        let mut shutdown = shutdown.fuse();
                        loop {
                            select! {
                                tick = tick_receiver.next() => {
                                    tick.unwrap();
                                    tick_count.fetch_add(1, Ordering::Relaxed);
                                    tick_confirmation_sender.unbounded_send(()).unwrap();
                                },
                                _ = shutdown => break,
                            }
                        }
                        shutdown_complete_sender.send(()).unwrap();
                    }
                })
                .unwrap();

            assert_eq!(tick_count.load(Ordering::Relaxed), 0);

            tick_sender.unbounded_send(()).unwrap();
            await!(tick_confirmation_receiver.next()).unwrap();
            assert_eq!(tick_count.load(Ordering::Relaxed), 1);

            tick_sender.unbounded_send(()).unwrap();
            await!(tick_confirmation_receiver.next()).unwrap();
            assert_eq!(tick_count.load(Ordering::Relaxed), 2);

            scope.shutdown();

            await!(shutdown_complete_receiver).unwrap();
            assert_eq!(tick_count.load(Ordering::Relaxed), 2);
        });
    }

    mod mocks {
        use {
            futures::{
                channel::oneshot,
                task::{Context, Poll},
                Future, FutureExt,
            },
            std::{
                ops::Drop,
                pin::Pin,
                sync::{
                    atomic::{AtomicUsize, Ordering},
                    Arc,
                },
            },
        };

        pub(super) struct TaskCounters {
            poll_call_count: Arc<AtomicUsize>,
            drop_call_count: Arc<AtomicUsize>,
        }

        impl TaskCounters {
            fn new() -> (Arc<AtomicUsize>, Arc<AtomicUsize>, Self) {
                let poll_call_count = Arc::new(AtomicUsize::new(0));
                let drop_call_count = Arc::new(AtomicUsize::new(0));

                (
                    poll_call_count.clone(),
                    drop_call_count.clone(),
                    Self { poll_call_count, drop_call_count },
                )
            }

            pub(super) fn poll_call(&self) -> usize {
                self.poll_call_count.load(Ordering::Relaxed)
            }

            pub(super) fn drop_call(&self) -> usize {
                self.drop_call_count.load(Ordering::Relaxed)
            }
        }

        pub(super) struct ImmediateTask {
            poll_call_count: Arc<AtomicUsize>,
            drop_call_count: Arc<AtomicUsize>,
            done_sender: Option<oneshot::Sender<()>>,
        }

        impl ImmediateTask {
            pub(super) fn new(done_sender: oneshot::Sender<()>) -> (TaskCounters, Self) {
                let (poll_call_count, drop_call_count, counters) = TaskCounters::new();
                (
                    counters,
                    Self { poll_call_count, drop_call_count, done_sender: Some(done_sender) },
                )
            }
        }

        impl Future for ImmediateTask {
            type Output = ();

            fn poll(mut self: Pin<&mut Self>, _cx: &mut Context) -> Poll<Self::Output> {
                self.poll_call_count.fetch_add(1, Ordering::Relaxed);

                if let Some(sender) = self.done_sender.take() {
                    sender.send(()).unwrap();
                }

                Poll::Ready(())
            }
        }

        impl Drop for ImmediateTask {
            fn drop(&mut self) {
                self.drop_call_count.fetch_add(1, Ordering::Relaxed);
            }
        }

        impl Unpin for ImmediateTask {}

        pub(super) struct ControlledTask {
            poll_call_count: Arc<AtomicUsize>,
            drop_call_count: Arc<AtomicUsize>,

            poll_sender: Option<oneshot::Sender<()>>,
            processing_complete: Option<oneshot::Receiver<()>>,
            drop_sender: Option<oneshot::Sender<()>>,
        }

        impl ControlledTask {
            pub(super) fn new(
                poll_sender: oneshot::Sender<()>,
                processing_complete: oneshot::Receiver<()>,
                drop_sender: oneshot::Sender<()>,
            ) -> (TaskCounters, Self) {
                let (poll_call_count, drop_call_count, counters) = TaskCounters::new();
                (
                    counters,
                    Self {
                        poll_call_count,
                        drop_call_count,
                        poll_sender: Some(poll_sender),
                        processing_complete: Some(processing_complete),
                        drop_sender: Some(drop_sender),
                    },
                )
            }
        }

        impl Future for ControlledTask {
            type Output = ();

            fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<Self::Output> {
                self.poll_call_count.fetch_add(1, Ordering::Relaxed);

                if let Some(sender) = self.poll_sender.take() {
                    sender.send(()).unwrap();
                }

                match self.processing_complete.take() {
                    Some(mut processing_complete) => match processing_complete.poll_unpin(cx) {
                        Poll::Ready(done) => done.unwrap(),
                        Poll::Pending => self.processing_complete = Some(processing_complete),
                    },
                    None => return Poll::Ready(()),
                }

                Poll::Pending
            }
        }

        impl Drop for ControlledTask {
            fn drop(&mut self) {
                self.drop_call_count.fetch_add(1, Ordering::Relaxed);

                if let Some(sender) = self.drop_sender.take() {
                    sender.send(()).unwrap();
                }
            }
        }

        impl Unpin for ControlledTask {}
    }
}
