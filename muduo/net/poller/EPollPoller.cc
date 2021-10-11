// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/poller/EPollPoller.h>
#include <muduo/base/Logging.h>
#include <muduo/net/Channel.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

// On Linux, the constants of poll(2) and epoll(4)
// are expected to be the same.
static_assert(EPOLLIN == POLLIN,        "epoll uses same flag values as poll");
static_assert(EPOLLPRI == POLLPRI,      "epoll uses same flag values as poll");
static_assert(EPOLLOUT == POLLOUT,      "epoll uses same flag values as poll");
static_assert(EPOLLRDHUP == POLLRDHUP,  "epoll uses same flag values as poll");
static_assert(EPOLLERR == POLLERR,      "epoll uses same flag values as poll");
static_assert(EPOLLHUP == POLLHUP,      "epoll uses same flag values as poll");

namespace
{
const int kNew = -1;
const int kAdded = 1;
const int kDeleted = 2;
}

EPollPoller::EPollPoller(EventLoop* loop)
  : Poller(loop),
    epollfd_(::epoll_create1(EPOLL_CLOEXEC)), // 创建epoll文件描述符
    events_(kInitEventListSize)
{
  if (epollfd_ < 0)
  {
    LOG_SYSFATAL << "EPollPoller::EPollPoller";
  }
}

EPollPoller::~EPollPoller()
{
  ::close(epollfd_); // 关闭epoll文件描述符
}

/******************************************************************** 
调用epoll_wait()获得当前活动的I/O事件，并调用
fillActiveChannels()找出有活动事件的fd，将其填充到activeChannels。
*********************************************************************/
Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
  LOG_TRACE << "fd total count " << channels_.size();
  // 调用epoll的等待函数，等待事件的发生，epoll_wait函数返回之后，events_中存放着已经发生的事件
  int numEvents = ::epoll_wait(epollfd_,
                               &*events_.begin(),
                               static_cast<int>(events_.size()),
                               timeoutMs);
  // 保存错误码
  int savedErrno = errno;
  // 当前时间
  Timestamp now(Timestamp::now());
  if (numEvents > 0)
  {
    LOG_TRACE << numEvents << " events happened";
    // 遍历事件列表，填充已激活事件处理器的列表
    fillActiveChannels(numEvents, activeChannels);
    // 调整事件列表的大小
    if (implicit_cast<size_t>(numEvents) == events_.size())
    {
      events_.resize(events_.size()*2);
    }
  }
  else if (numEvents == 0)
  {
    LOG_TRACE << "nothing happened";
  }
  else
  {
    // error happens, log uncommon ones
    if (savedErrno != EINTR)
    {
      errno = savedErrno;
      LOG_SYSERR << "EPollPoller::poll()";
    }
  }
  return now;
}

/******************************************************************** 
遍历事件列表，找出有活动事件的fd，把它对应的channel
填入activeChannels。
*********************************************************************/
void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const
{
  assert(implicit_cast<size_t>(numEvents) <= events_.size());
  // 把已经激活事件的事件处理器添加到已激活事件处理器列表中
  for (int i = 0; i < numEvents; ++i)
  {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
#ifndef NDEBUG
    int fd = channel->fd();
    ChannelMap::const_iterator it = channels_.find(fd);
    assert(it != channels_.end());
    assert(it->second == channel);
#endif
    // events_[i].events中存放了发生的事件，在通道中设置事件
    channel->set_revents(events_[i].events);
    activeChannels->push_back(channel);
  }
}

/******************************************************************** 
更新事件处理器，操作的方式取决于Channel的index字段，
默认是-1，表示新。
*********************************************************************/
void EPollPoller::updateChannel(Channel* channel)
{
  Poller::assertInLoopThread();
  // 返回某一个channel对应在channels_的下标
  const int index = channel->index();
  LOG_TRACE << "fd = " << channel->fd()
    << " events = " << channel->events() << " index = " << index;
  if (index == kNew || index == kDeleted)
  {
    int fd = channel->fd();
    // 如果是新增事件，把事件处理器存放到channels_（是一个map）中
    if (index == kNew)
    {
      assert(channels_.find(fd) == channels_.end());
      channels_[fd] = channel;
    }
    else
    {
      assert(channels_.find(fd) != channels_.end());
      assert(channels_[fd] == channel);
    }
    // 更新文件描述符（epoll专用的文件描述符）
    channel->set_index(kAdded);
    update(EPOLL_CTL_ADD, channel);
  }
  // 如果是删除或修改
  else
  {
    int fd = channel->fd();
    (void)fd;
    assert(channels_.find(fd) != channels_.end());
    assert(channels_[fd] == channel);
    assert(index == kAdded);
    // 删除文件描述符
    if (channel->isNoneEvent())
    {
      update(EPOLL_CTL_DEL, channel);
      channel->set_index(kDeleted);
    }
    // 修改文件描述符
    else
    {
      update(EPOLL_CTL_MOD, channel);
    }
  }
}

/******************************************************************** 
删除事件处理器。
*********************************************************************/
void EPollPoller::removeChannel(Channel* channel)
{
  // 判断是否在IO线
  Poller::assertInLoopThread();
  int fd = channel->fd();
  LOG_TRACE << "fd = " << fd;
  assert(channels_.find(fd) != channels_.end());
  assert(channels_[fd] == channel);
  assert(channel->isNoneEvent());
  // 获得pfd位置的索引
  int index = channel->index();
  assert(index == kAdded || index == kDeleted);
  // 在map中删除channel
  size_t n = channels_.erase(fd);
  (void)n;
  assert(n == 1);
  if (index == kAdded)
  {
    update(EPOLL_CTL_DEL, channel);
  }
  channel->set_index(kNew);
}

/******************************************************************** 
调用epoll_ctl()注册要监听的事件类型。
*********************************************************************/
void EPollPoller::update(int operation, Channel* channel)
{
  // 构建一个epoll事件
  struct epoll_event event;
  memZero(&event, sizeof event);
  // 存放事件处理器中需要处理的事件和事件处理器
  event.events = channel->events();
  event.data.ptr = channel;
  // 事件处理器对应的描述符
  int fd = channel->fd();
  LOG_TRACE << "epoll_ctl op = " << operationToString(operation)
    << " fd = " << fd << " event = { " << channel->eventsToString() << " }";
  // epoll的事件注册函数
  if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
  {
    if (operation == EPOLL_CTL_DEL) // 从epfd中删除一个fd
    {
      LOG_SYSERR << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
    else
    {
      LOG_SYSFATAL << "epoll_ctl op =" << operationToString(operation) << " fd =" << fd;
    }
  }
}

// 转换为字符串
const char* EPollPoller::operationToString(int op)
{
  switch (op)
  {
    case EPOLL_CTL_ADD:
      return "ADD";
    case EPOLL_CTL_DEL:
      return "DEL";
    case EPOLL_CTL_MOD:
      return "MOD";
    default:
      assert(false && "ERROR op");
      return "Unknown Operation";
  }
}
