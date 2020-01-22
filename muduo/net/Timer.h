// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_TIMER_H
#define MUDUO_NET_TIMER_H

#include "muduo/base/Atomic.h"
#include "muduo/base/Timestamp.h"
#include "muduo/net/Callbacks.h"

namespace muduo
{
namespace net
{

// zhou: not only describe a Timer parameters, but also can handle the repeat.
//       sequence is used to avoid ABA problem. "Timer *" is not enough to confirm
//       the object is expected.

///
/// Internal class for timer event.
///
class Timer : noncopyable
{
 public:
  // zhou: "typedef std::function<void()> TimerCallback;"
  Timer(TimerCallback cb, Timestamp when, double interval)
    : callback_(std::move(cb)),
      expiration_(when),
      interval_(interval),
      repeat_(interval > 0.0),
      sequence_(s_numCreated_.incrementAndGet())
  { }

  void run() const
  {
    callback_();
  }

  Timestamp expiration() const  { return expiration_; }
  bool repeat() const { return repeat_; }
  int64_t sequence() const { return sequence_; }

  void restart(Timestamp now);

  static int64_t numCreated() { return s_numCreated_.get(); }

 private:
  const TimerCallback callback_;
  Timestamp expiration_;
  const double interval_;
  const bool repeat_;
  const int64_t sequence_;

  // zhou: static member variable, must be defined and init outside of class definition.
  //       used to get timer ID, unique globally.
  static AtomicInt64 s_numCreated_;
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_TIMER_H
