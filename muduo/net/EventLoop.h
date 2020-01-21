// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_EVENTLOOP_H
#define MUDUO_NET_EVENTLOOP_H

#include <atomic>
#include <functional>
#include <vector>

#include <boost/any.hpp>

#include "muduo/base/Mutex.h"
#include "muduo/base/CurrentThread.h"
#include "muduo/base/Timestamp.h"
#include "muduo/net/Callbacks.h"
#include "muduo/net/TimerId.h"

namespace muduo
{
namespace net
{

// zhou: only used to define pointer, and forward declaration.
//       By this way to avoid includes their header files.
class Channel;
class Poller;
class TimerQueue;

// zhou: the thread which created EventLoop, is IO thread. Its lifttime is forever.

///
/// Reactor, at most one per thread.
///
/// This is an interface class, so don't expose too much details.
class EventLoop : noncopyable
{
public:
  // zhou: could be used to refer to any callable object
  typedef std::function<void()> Functor;

  EventLoop();
  ~EventLoop();  // force out-line dtor, for std::unique_ptr members.

  ///
  /// Loops forever.
  ///
  /// Must be called in the same thread as creation of the object.
  ///
  void loop();

  /// Quits loop.
  ///
  /// This is not 100% thread safe, if you call through a raw pointer,
  /// better to call through shared_ptr<EventLoop> for 100% safety.
  void quit();

  ///
  /// Time when poll returns, usually means data arrival.
  ///
  Timestamp pollReturnTime() const { return pollReturnTime_; }

  int64_t iteration() const { return iteration_; }

  /// Runs callback immediately in the loop thread.
  /// It wakes up the loop, and run the cb.
  /// If in the same loop thread, cb is run within the function.
  /// Safe to call from other threads.
  void runInLoop(Functor cb);
  /// Queues callback in the loop thread.
  /// Runs after finish pooling.
  /// Safe to call from other threads.
  void queueInLoop(Functor cb);

  size_t queueSize() const;

  // timers

  ///
  /// Runs callback at 'time'.
  /// Safe to call from other threads.
  ///
  TimerId runAt(Timestamp time, TimerCallback cb);
  ///
  /// Runs callback after @c delay seconds.
  /// Safe to call from other threads.
  ///
  TimerId runAfter(double delay, TimerCallback cb);
  ///
  /// Runs callback every @c interval seconds.
  /// Safe to call from other threads.
  ///
  TimerId runEvery(double interval, TimerCallback cb);
  ///
  /// Cancels the timer.
  /// Safe to call from other threads.
  ///
  void cancel(TimerId timerId);

  // internal usage
  void wakeup();
  void updateChannel(Channel* channel);
  void removeChannel(Channel* channel);
  bool hasChannel(Channel* channel);

  // pid_t threadId() const { return threadId_; }
  void assertInLoopThread()
  {
    if (!isInLoopThread())
    {
      abortNotInLoopThread();
    }
  }

  // zhou:
  bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

  // bool callingPendingFunctors() const { return callingPendingFunctors_; }
  bool eventHandling() const { return eventHandling_; }

  void setContext(const boost::any& context)
  { context_ = context; }

  const boost::any& getContext() const
  { return context_; }

  boost::any* getMutableContext()
  { return &context_; }

  static EventLoop* getEventLoopOfCurrentThread();

 private:
  void abortNotInLoopThread();
  void handleRead();  // waked up
  void doPendingFunctors();

  void printActiveChannels() const; // DEBUG

  // zhou: socket handler list.
  typedef std::vector<Channel*> ChannelList;

  bool looping_; /* atomic */
  std::atomic<bool> quit_;
  // zhou: flag for handle channel event.
  bool eventHandling_; /* atomic */
  // zhou: flag for handle Task Queue.
  //       only be evaluated in EvenLoop owner thread. So it is safe to use
  //       normal bool type. Otherwise, we have to use std::atomic<bool>.
  bool callingPendingFunctors_; /* atomic */

  int64_t iteration_;
  // zhou: ??? used for
  const pid_t threadId_;
  Timestamp pollReturnTime_;

  std::unique_ptr<Poller> poller_;

  // zhou: will be inited in ctor.
  //       Any timer want be executed in this thread, should be managed by it.
  std::unique_ptr<TimerQueue> timerQueue_;

  // zhou: used to wait up a EventLoop which is blocked, from another thread.
  int wakeupFd_;

  // zhou: this channel init depends on "wakeupFd_", so must be after it.
  // unlike in TimerQueue, which is an internal class,
  // we don't expose Channel to client.
  std::unique_ptr<Channel> wakeupChannel_;

  // zhou: used to refer to any type of object.
  boost::any context_;

  // zhou: socket handler list
  // scratch variables
  ChannelList activeChannels_;

  // zhou: just avoid remove channel which is processing.
  Channel* currentActiveChannel_;

  // zhou: used to protect "pendingFunctors_"
  mutable MutexLock mutex_;
  // zhou: Task list, could be queued from other EventLoop.
  //       GUARDED_BY(mutex_), Promise the member will be protected by "mutext_".
  //       Once broken, it will be detected by Clang.
  std::vector<Functor> pendingFunctors_ GUARDED_BY(mutex_);
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_EVENTLOOP_H
