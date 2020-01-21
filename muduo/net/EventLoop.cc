// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/EventLoop.h"

#include "muduo/base/Logging.h"
#include "muduo/base/Mutex.h"
#include "muduo/net/Channel.h"
#include "muduo/net/Poller.h"
#include "muduo/net/SocketsOps.h"
#include "muduo/net/TimerQueue.h"

#include <algorithm>

#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

// zhou: anomynous namespace, used to replace static variable and fucntions.
//       All functions and variable are file local visibility.
namespace
{
// zhou: refer to the EventLoop object within this thread.
//       Only one EventLoop object can be created.
__thread EventLoop* t_loopInThisThread = 0;

// zhou: block for no more than 10s.
const int kPollTimeMs = 10000;

int createEventfd()
{
  // zhou: when refer to global namespace.
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0)
  {
    LOG_SYSERR << "Failed in eventfd";
    abort();
  }
  return evtfd;
}

#pragma GCC diagnostic ignored "-Wold-style-cast"
class IgnoreSigPipe
{
 public:
  IgnoreSigPipe()
  {
    ::signal(SIGPIPE, SIG_IGN);
    // LOG_TRACE << "Ignore SIGPIPE";
  }
};
#pragma GCC diagnostic error "-Wold-style-cast"

IgnoreSigPipe initObj;
}  // namespace


////////////////////////////////////////////////////////////////////////////////

EventLoop* EventLoop::getEventLoopOfCurrentThread()
{
  return t_loopInThisThread;
}

// zhou: EventLoop is way to manage Task Queue, scheduler framework.
EventLoop::EventLoop()
  : looping_(false),
    quit_(false),
    eventHandling_(false),
    callingPendingFunctors_(false),
    iteration_(0),
    threadId_(CurrentThread::tid()),
    poller_(Poller::newDefaultPoller(this)),
    timerQueue_(new TimerQueue(this)),
    wakeupFd_(createEventfd()),
    wakeupChannel_(new Channel(this, wakeupFd_)),
    currentActiveChannel_(NULL)
{
  LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;

  // zhou: make sure only one EventLoop will be created for each thread.
  if (t_loopInThisThread)
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this;
  }

  wakeupChannel_->setReadCallback(
      std::bind(&EventLoop::handleRead, this));

  // we are always reading the wakeupfd
  wakeupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
  LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
            << " destructs in thread " << CurrentThread::tid();
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
  t_loopInThisThread = NULL;
}

// zhou: not busy loop, but will run forever.
void EventLoop::loop()
{
  // zhou: prevent from recursive invoking loop().
  assert(!looping_);

  // zhou: must run in same thread as creation of the object.
  assertInLoopThread();

  looping_ = true;
  quit_ = false;  // FIXME: what if someone calls quit() before loop() ?

  LOG_TRACE << "EventLoop " << this << " start looping";

  while (!quit_)
  {
    activeChannels_.clear();

    // zhou: use "::timerfd_create()" to manage timer, "kPollTimeMs" do nothing.
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);

    ++iteration_;
    if (Logger::logLevel() <= Logger::TRACE)
    {
      printActiveChannels();
    }

    // TODO sort channel by priority
    eventHandling_ = true;
    for (Channel* channel : activeChannels_)
    {
      currentActiveChannel_ = channel;
      currentActiveChannel_->handleEvent(pollReturnTime_);
    }
    currentActiveChannel_ = NULL;
    eventHandling_ = false;

    // zhou: handle Task Queue
    doPendingFunctors();
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}

void EventLoop::quit()
{
  quit_ = true;
  // There is a chance that loop() just executes while(!quit_) and exits,
  // then EventLoop destructs, then we are accessing an invalid object.
  // Can be fixed using mutex_ in both places.
  if (!isInLoopThread())
  {
    wakeup();
  }
}

// zhou: "Runs callback immediately in the loop thread.
//        It wakes up the loop, and run the cb.
//        If in the same loop thread, cb is run within the function.
//        Safe to call from other threads."
void EventLoop::runInLoop(Functor cb)
{
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
    queueInLoop(std::move(cb));
  }
}

// zhou: "Queues callback in the loop thread. Runs after finish pooling.
//        Safe to call from other threads."
void EventLoop::queueInLoop(Functor cb)
{
  {
  MutexLockGuard lock(mutex_);
  // zhou: 'cb' is normally a Functor object returned by std::bind().
  //       In order to avoid Copy Ctor, using Move Ctor.
  pendingFunctors_.push_back(std::move(cb));
  }

  // zhou: "callingPendingFunctors_" will only be evaluted when
  //        "isInLoopThread()" is true which means in EventLoop thread.
  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();
  }
}

size_t EventLoop::queueSize() const
{
  MutexLockGuard lock(mutex_);
  return pendingFunctors_.size();
}

// zhou: why there is no user context within TimerCallback?
//       Because std::bind may already set it.
TimerId EventLoop::runAt(Timestamp time, TimerCallback cb)
{
  return timerQueue_->addTimer(std::move(cb), time, 0.0);
}


TimerId EventLoop::runAfter(double delay, TimerCallback cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timerId)
{
  return timerQueue_->cancel(timerId);
}

// zhou: used by poller to register Channel.
void EventLoop::updateChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();

  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();

  if (eventHandling_)
  {
    // zhou: can NOT remove channel when handling it.
    assert(currentActiveChannel_ == channel ||
        std::find(activeChannels_.begin(), activeChannels_.end(), channel) == activeChannels_.end());
  }

  // zhou: remove from epoll.
  poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();

  return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " <<  CurrentThread::tid();
}

// zhou: each time wakeup() invoked, will +1 eventfd internal counter.
//
void EventLoop::wakeup()
{
  uint64_t one = 1;
  ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);

  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

// zhou: handle read event (wake up here)
void EventLoop::handleRead()
{
  uint64_t one = 1;

  // zhou: one will be changed.
  ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

// zhou: handle Task Queue.
void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  {
    // zhou: move all pending tasks to new temp vector "functors".
    //       By this way, to make the race condition as small as possible.
    MutexLockGuard lock(mutex_);
    functors.swap(pendingFunctors_);
  }

  // zhou: should limit time spending on the Task Queue. Otherwise, may cause
  //       high priority events from channel lost.
  for (const Functor& functor : functors)
  {
    functor();
  }
  callingPendingFunctors_ = false;
}

void EventLoop::printActiveChannels() const
{
  for (const Channel* channel : activeChannels_)
  {
    LOG_TRACE << "{" << channel->reventsToString() << "} ";
  }
}
