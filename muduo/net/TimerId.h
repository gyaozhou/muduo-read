// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_TIMERID_H
#define MUDUO_NET_TIMERID_H

#include "muduo/base/copyable.h"

namespace muduo
{
namespace net
{

class Timer;

// zhou: copyable, work like a std::weak_ptr<Timer>, have to check TimerQueue and
//       make sure "timer_" is still valid before use it.
//       TimerId will be preserved by user.

///
/// An opaque identifier, for canceling Timer.
///
class TimerId : public muduo::copyable
{
 public:
  // zhou: use 0 for invalid TimerId?
  TimerId()
    : timer_(NULL),
      sequence_(0)
  {
  }

  TimerId(Timer* timer, int64_t seq)
    : timer_(timer),
      sequence_(seq)
  {
  }

  // zhou: we want a default copy-ctor and assignment-ctor.

  // default copy-ctor, dtor and assignment are okay

  friend class TimerQueue;

 private:
  Timer* timer_;

  // zhou: Althrough Timer includes "sequence_", but due to ABA problem, when we
  //       want to cancel timer, Timer* is not safe enough, user perserved
  //       sequence (within TimerId) helps to discriminate the correct Timer.
  int64_t sequence_;
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_TIMERID_H
