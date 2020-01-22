// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include "muduo/net/TimerQueue.h"

#include "muduo/base/Logging.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/Timer.h"
#include "muduo/net/TimerId.h"

#include <sys/timerfd.h>
#include <unistd.h>

namespace muduo
{
namespace net
{
namespace detail
{

int createTimerfd()
{
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0)
  {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}

struct timespec howMuchTimeFromNow(Timestamp when)
{
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();
  if (microseconds < 100)
  {
    microseconds = 100;
  }
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

// zhou: like eventfd, just need to know something happened, ignore the content.
void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

// zhou:
// "new_value.it_value specifies the initial expiration of the timer, in seconds
//  and nanoseconds.  Setting either field of new_value.it_value to a nonzero value
//  arms the timer.  Setting both fields of new_value.it_value to zero disarms the timer.
//
//  Setting  one  or  both  fields of new_value.it_interval to nonzero values
//  specifies the period, in seconds and nanoseconds, for repeated timer expirations
//  after the initial expiration.  If both fields of new_value.it_interval are zero,
//  the timer expires just
//  once, at the time specified by new_value.it_value.
//
//  By default, the initial expiration time specified in new_value is interpreted
//  relative to the current time on the timer's clock at the time of the call
//  (i.e., new_value.it_value specifies a time relative to  the  current  value  of
//  the  clock  specified  by clockid).  An absolute timeout can be selected via
//  the flags argument."

void resetTimerfd(int timerfd, Timestamp expiration)
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;
  struct itimerspec oldValue;
  memZero(&newValue, sizeof newValue);
  memZero(&oldValue, sizeof oldValue);

  newValue.it_value = howMuchTimeFromNow(expiration);

  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
  if (ret)
  {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}  // namespace detail
}  // namespace net
}  // namespace muduo


////////////////////////////////////////////////////////////////////////////////

using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

// zhou: the member variables init value, not only comes from ctor parameters.
TimerQueue::TimerQueue(EventLoop* loop)
  : loop_(loop),
    timerfd_(createTimerfd()),
    timerfdChannel_(loop, timerfd_),
    timers_(),
    callingExpiredTimers_(false)
{
  // zhou: only readable event will happen, with the fd timerfd_create(2).
  //       No writeable/close/error event will happen.
  timerfdChannel_.setReadCallback(
      std::bind(&TimerQueue::handleRead, this));

  // zhou: disarm means stop the timer
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();
}

// zhou: "timerQueue_" is a member of EventLoop, it happens in the EventLoop thread.
TimerQueue::~TimerQueue()
{
  timerfdChannel_.disableAll();
  timerfdChannel_.remove();

  ::close(timerfd_);

  // zhou: release all created Timer.
  //       Channel is a member of TimerQueue

  // do not remove channel, since we're in EventLoop::dtor();
  for (const Entry& timer : timers_)
  {
    // zhou: "typedef std::pair<Timestamp, Timer*> Entry;"
    delete timer.second;
  }
}

// zhou: create a Task, append to EventLoop Task Queue. In case the timer is
//       closed to now. It may expired before executed.
TimerId TimerQueue::addTimer(TimerCallback cb,
                             Timestamp when,
                             double interval)
{
  // zhou: TimerQueue will manage the life cycle of "timer".
  Timer* timer = new Timer(std::move(cb), when, interval);

  // zhou: EventLoop::runInLoop, required a functor "void()".
  //       The ownership of "timer" is transferred to functor.
  //       And move to "timers" finally.
  loop_->runInLoop(
      std::bind(&TimerQueue::addTimerInLoop, this, timer));

  // zhou: at this time, the timer is still not be queued into timer queue,
  //       but the TimeId returned.
  return TimerId(timer, timer->sequence());
}

void TimerQueue::cancel(TimerId timerId)
{
  loop_->runInLoop(
      std::bind(&TimerQueue::cancelInLoop, this, timerId));
}

// zhou: this function will be invoked in EventLoop local thread.
void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();

  bool earliestChanged = insert(timer);

  // zhou: have to operate "timerfd_"
  if (earliestChanged)
  {
    // zhou: No matter once current earliest timer expired right now, since the way
    //       getExpired() used.
    resetTimerfd(timerfd_, timer->expiration());
  }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());

  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  ActiveTimerSet::iterator it = activeTimers_.find(timer);

  // zhou: user preserved a timer already been released, maybe already be expired just now,
  //       maybe user preserved a dangling pointer, ...
  if (it != activeTimers_.end())
  {
    // zhou: "typedef std::pair<Timestamp, Timer*> Entry;"
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));

    assert(n == 1); (void)n;

    delete it->first; // FIXME: no delete please
    activeTimers_.erase(it);
  }
  else if (callingExpiredTimers_)
  {
    // zhou: due to cancelInLoop() may be invoked in timer expired callback function.
    cancelingTimers_.insert(timer);
  }

  assert(timers_.size() == activeTimers_.size());
}


void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();

  Timestamp now(Timestamp::now());

  // zhou: don't care about what we read from "timerfd_"
  readTimerfd(timerfd_, now);

  // zhou: MUST be some timers expired
  std::vector<Entry> expired = getExpired(now);

  callingExpiredTimers_ = true;
  cancelingTimers_.clear();

  // safe to callback outside critical section
  for (const Entry& it : expired)
  {
    // zhou: "typedef std::pair<Timestamp, Timer*> Entry;", invoke callback
    //       one by one.
    it.second->run();
  }
  callingExpiredTimers_ = false;

  // zhou: process deferred cancel operations
  reset(expired, now);
}

// zhou: according current time, get a list of expired timers.
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  assert(timers_.size() == activeTimers_.size());

  std::vector<Entry> expired;
  // zhou: it's better to make the sentry (now + small time)
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX));
  TimerList::iterator end = timers_.lower_bound(sentry);
  assert(end == timers_.end() || now < end->first);

  // zhou: move all expired timers to "expired"
  std::copy(timers_.begin(), end, back_inserter(expired));
  timers_.erase(timers_.begin(), end);

  for (const Entry& it : expired)
  {
    ActiveTimer timer(it.second, it.second->sequence());
    size_t n = activeTimers_.erase(timer);
    assert(n == 1); (void)n;
  }

  assert(timers_.size() == activeTimers_.size());

  // zhou: RVO will secure the performance, similar with std::move() but better
  //       in this case.
  return expired;
}

// zhou: used after expired.
void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;

  for (const Entry& it : expired)
  {
    ActiveTimer timer(it.second, it.second->sequence());

    // zhou: a repeatable timer expired but has not been cancelled, restart again.
    //       Otherwise, release it.
    if (it.second->repeat()
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
      it.second->restart(now);
      insert(it.second);
    }
    else
    {
      // FIXME move to a free list
      delete it.second; // FIXME: no delete please
    }
  }

  // zhou: find next to be expired.
  if (!timers_.empty())
  {
    nextExpire = timers_.begin()->second->expiration();
  }

  if (nextExpire.valid())
  {
    resetTimerfd(timerfd_, nextExpire);
  }
}

// zhou: why always check "assert(timers_.size() == activeTimers_.size());" ?
//       Because cancel timer implementation, TimerQueue has to maintains two
//       std::set, ("timers_", timestamp order, "activeTimers_" to cancel).
//       So, once these two std::set mismatch, mistake happened.
bool TimerQueue::insert(Timer* timer)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());

  bool earliestChanged = false;

  Timestamp when = timer->expiration();
  TimerList::iterator it = timers_.begin();
  if (it == timers_.end() || when < it->first)
  {
    // zhou: the new timer will be the first expired timer.
    earliestChanged = true;
  }

  // zhou: insert to "timers_"
  {
    // zhou: std::set<T> insert() will always return
    //       std::pair<std::set<T>::interator, bool>
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));

    // zhou: should always success
    assert(result.second); (void)result;
  }

  // zhou: insert to "activeTimers_"
  {
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));

    assert(result.second); (void)result;
  }

  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;
}
