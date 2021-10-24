// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoopThread.h>
#include <muduo/net/EventLoop.h>

using namespace muduo;
using namespace muduo::net;

/******************************************************************** 
这个类专门创建一个线程用于执行Reactor的事件循环，当然
这只是一个辅助类，没有说一定要使用它，可以根据自己的情况进行选择，
也可以不创建线程去执行事件循环，而在主线程中执行事件循环，
一切根据自己的需要。

EventLoopThread（也叫IO线程）的工作流程为：
1、在主线程（暂且这么称呼）创建EventLoopThread对象。 
2、主线程调用EventLoopThread.start()，启动EventLoopThread中的线程
（称为IO线程），这是主线程要等待IO线程创建完成EventLoop对象。 
3、IO线程调用threadFunc创建EventLoop对象。通知主线程已经创建完成。 
4、主线程返回创建的EventLoop对象。
*********************************************************************/
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const string& name)
  : loop_(NULL),
    exiting_(false),
    thread_(std::bind(&EventLoopThread::threadFunc, this), name),
    mutex_(),
    cond_(mutex_),
    callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
  exiting_ = true;
  if (loop_ != NULL)
  {
    loop_->quit();
    thread_.join();
  }
}

/******************************************************************** 
启动一个EventLoop线程。
*********************************************************************/
EventLoop* EventLoopThread::startLoop()
{
  assert(!thread_.started());
  // 当前线程启动，调用threadFunc()
  thread_.start();
  EventLoop* loop = NULL;
  {
    MutexLockGuard lock(mutex_);
    while (loop_ == NULL)
    {
      // 等待创建好当前IO线程
      cond_.wait();
    }
    loop = loop_;
  }
  return loop;
}

void EventLoopThread::threadFunc()
{
  EventLoop loop;
  // 如果有初始化函数，就先调用初始化函数
  if (callback_)
  {
    callback_(&loop);
  }
  {
    MutexLockGuard lock(mutex_);
    loop_ = &loop;
    // 通知startLoop线程已经启动完毕
    cond_.notify();
  }
  // 事件循环
  loop.loop();
  MutexLockGuard lock(mutex_);
  loop_ = NULL;
}
