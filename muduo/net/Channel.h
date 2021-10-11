#ifndef MUDUO_NET_CHANNEL_H
#define MUDUO_NET_CHANNEL_H

#include <muduo/base/noncopyable.h>
#include <muduo/base/Timestamp.h>
#include <functional>
#include <memory>

namespace muduo
{
namespace net
{

class EventLoop;

/******************************************************************** 
channel类（事件处理器）负责注册与响应I/O事件，但是它不
拥有文件描述符。每一个channel对象自始至终都只属于一个EventLoop。
*********************************************************************/
class Channel : noncopyable
{
 public:
  // 事件回调函数
  typedef std::function<void()> EventCallback;
  // 读事件回调函数
  typedef std::function<void(Timestamp)> ReadEventCallback;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  // 处理事件
  void handleEvent(Timestamp receiveTime);
  // 设置读回调函数（参数是TcpConnection注册的）
  void setReadCallback(ReadEventCallback cb)
  { readCallback_ = std::move(cb); }
  // 设置写回调函数
  void setWriteCallback(EventCallback cb)
  { writeCallback_ = std::move(cb); }
  // 设置关闭回调函数
  void setCloseCallback(EventCallback cb)
  { closeCallback_ = std::move(cb); }
  // 设置错误处理回调函数
  void setErrorCallback(EventCallback cb)
  { errorCallback_ = std::move(cb); }

  // 把当前事件处理器绑定到某一个对象上
  void tie(const std::shared_ptr<void>&);

  // 返回文件描述符
  int fd() const { return fd_; }

  // 返回该事件处理所需要处理的事件
  int events() const { return events_; }
  // 设置实际活动的事件
  void set_revents(int revt) { revents_ = revt; }

  // 判断是否有事件
  bool isNoneEvent() const { return events_ == kNoneEvent; }

  // 启用读（按位或后赋值），然后更新通道中的事件
  void enableReading() { events_ |= kReadEvent; update(); }
  // 禁用读（按位与后赋值）
  void disableReading() { events_ &= ~kReadEvent; update(); }
  // 启用写
  void enableWriting() { events_ |= kWriteEvent; update(); }
  // 禁用读
  void disableWriting() { events_ &= ~kWriteEvent; update(); }
  // 启用所有
  void disableAll() { events_ = kNoneEvent; update(); }

  // 是否正在写
  bool isWriting() const { return events_ & kWriteEvent; }
  // 是否正在读
  bool isReading() const { return events_ & kReadEvent; }

  // 返回索引
  int index() { return index_; }
  // 设置索引
  void set_index(int idx) { index_ = idx; }

  // 用于调试，把事件转换为字符串
  string reventsToString() const;
  string eventsToString() const;

  // 不记录hup事件
  void doNotLogHup() { logHup_ = false; }

  // 所属的事件循环（一个EventLoop可以有多个channel，但是一个channel只属于一个EventLoop）
  EventLoop* ownerLoop() { return loop_; }

  // 从事件循环对象中把自己删除
  void remove();

 private:
  static string eventsToString(int fd, int ev);

  // 更新
  void update();

  // 处理事件
  void handleEventWithGuard(Timestamp receiveTime);

  // 事件标记
  static const int kNoneEvent;
  static const int kReadEvent;
  static const int kWriteEvent;

  EventLoop* loop_;
  const int  fd_;
  int        events_;  // 关心的事件
  int        revents_; // 实际活动的事件
  int        index_;   // 表示在poll事件数组中的序号
  bool       logHup_;

  std::weak_ptr<void> tie_;
  bool tied_;
  bool eventHandling_; // 是否正在处理事件
  bool addedToLoop_; // 是否已经被添加到事件循环中
  // 事件回调
  ReadEventCallback readCallback_;
  EventCallback writeCallback_;
  EventCallback closeCallback_;
  EventCallback errorCallback_;
};

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_CHANNEL_H