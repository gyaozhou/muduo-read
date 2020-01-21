// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/EventLoopThread.h"

#include "muduo/net/EventLoop.h"

using namespace muduo;
using namespace muduo::net;

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const string& name)
  : loop_(NULL),
    exiting_(false),
    // zhou: the class member could use "this" object, design pattern.
    thread_(std::bind(&EventLoopThread::threadFunc, this), name),
    mutex_(),
    cond_(mutex_),
    callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
  exiting_ = true;
  if (loop_ != NULL) // not 100% race-free, eg. threadFunc could be running callback_.
  {
    // still a tiny chance to call destructed object, if threadFunc exits just now.
    // but when EventLoopThread destructs, usually programming is exiting anyway.
    loop_->quit();
    thread_.join();
  }
}

// zhou: public API
EventLoop* EventLoopThread::startLoop()
{
  assert(!thread_.started());

  // zhou: this function will return when new thread started,
  //       which runs "EventLoopThread::threadFunc()"
  thread_.start();

  // zhou: wait for EventLoop set up in new thread.
  EventLoop* loop = NULL;
  {
    MutexLockGuard lock(mutex_);
    while (loop_ == NULL)
    {
      cond_.wait();
    }
    loop = loop_;
  }

  return loop;
}

// zhou: private main function of "thread_"
void EventLoopThread::threadFunc()
{
  // zhou: function local variable satisfied the requirement,
  //       since this function never return.
  EventLoop loop;

  // zhou: when not actual function providered, it will be false.
  if (callback_)
  {
    // zhou: init EventLoop
    callback_(&loop);
  }

  {
    MutexLockGuard lock(mutex_);
    loop_ = &loop;
    cond_.notify();
  }

  // zhou: work here forever.
  loop.loop();

  //assert(exiting_);
  MutexLockGuard lock(mutex_);
  loop_ = NULL;
}
