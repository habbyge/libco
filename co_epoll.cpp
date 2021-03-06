/*
* Tencent is pleased to support the open source community by making Libco
available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "co_epoll.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__APPLE__) && !defined(__FreeBSD__)

int co_epoll_wait(int epfd, struct co_epoll_res* events, int maxevents, int timeout) {
  return epoll_wait(epfd, events->events, maxevents, timeout);
}
int co_epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
  return epoll_ctl(epfd, op, fd, ev);
}
int co_epoll_create(int size) { return epoll_create(size); }

struct co_epoll_res* co_epoll_res_alloc(int n) {
  struct co_epoll_res* ptr = (struct co_epoll_res*) malloc(sizeof(struct co_epoll_res));
  ptr->size = n;
  ptr->events = (struct epoll_event*) calloc(1, n * sizeof(struct epoll_event));

  return ptr;
}
void co_epoll_res_free(struct co_epoll_res* ptr) {
  if (!ptr)
    return;
  if (ptr->events)
    free(ptr->events);
  free(ptr);
}

#else

class clsFdMap { // million of fd, 1024 * 1024, 1M
private:
  static const int row_size = 1024;
  static const int col_size = 1024;

  void** m_pp[1024];

public:
  clsFdMap() {
    memset(m_pp, 0, sizeof(m_pp));
  }

  ~clsFdMap() {
    for (int i = 0; i < sizeof(m_pp) / sizeof(m_pp[0]); i++) {
      if (m_pp[i]) {
        free(m_pp[i]);
        m_pp[i] = NULL;
      }
    }
  }

  inline int clear(int fd) {
    set(fd, NULL);
    return 0;
  }

  inline int set(int fd, const void* ptr) {
    int idx = fd / row_size;
    if (idx < 0 || idx >= sizeof(m_pp) / sizeof(m_pp[0])) {
      assert(__LINE__ == 0);
      return -__LINE__;
    }
    if (!m_pp[idx]) {
      m_pp[idx] = (void**) calloc(1, sizeof(void*) * col_size);
    }
    m_pp[idx][fd % col_size] = (void*) ptr;
    return 0;
  }

  inline void* get(int fd) {
    int idx = fd / row_size;
    if (idx < 0 || idx >= sizeof(m_pp) / sizeof(m_pp[0])) {
      return NULL;
    }
    void** lp = m_pp[idx];
    if (!lp)
      return NULL;

    return lp[fd % col_size];
  }
};

__thread clsFdMap* s_fd_map = NULL;

static inline clsFdMap* get_fd_map() {
  if (!s_fd_map) {
    s_fd_map = new clsFdMap();
  }
  return s_fd_map;
}

struct kevent_pair_t {
  int fire_idx;
  int events;
  uint64_t u64;
};

/**
 * kqueue与epoll非常相似，最初是2000年Jonathan Lemon在FreeBSD系统上开发的一个高性能的事件通知接口
 * 注册一批socket描述符到 kqueue 以后，当其中的描述符状态发生变化时，kqueue 将一次性通知应用程序哪些
 * 描述符可读、可写或出错了
 * kqueue的接口包括 kqueue()、kevent() 两个系统调用和 struct kevent 结构，在event.h头文件中：
 * 1、kqueue() 生成一个内核事件队列，返回该队列的文件描述符。其它 API 通过该描述符操作这个 kqueue
 * 2、kevent() 提供向内核注册 / 反注册事件和返回就绪事件或错误事件
 * 3、struct kevent 就是kevent()操作的最基本的事件结构
 */
int co_epoll_create(int size) {
  // kqueue 是一种可扩展的事件通知接口，kqueue在内核与用户空间之间充当输入输出事件的管线，因此在
  // 事件循环的迭代中，进行一次 kevent(2) 系统调用不仅可以接收未决事件，还可以修改事件过滤器

  // 为什么epoll和kqueue可以用基于事件的方式，单线程的实现并发？
  // kqueue() - 生成一个内核事件队列，返回该队列的文件描述符。其它 API 通过该描述符操作这个 kqueue。
  // kevent() - 提供向内核注册/反注册事件和返回就绪事件或错误事件
  // struct kevent 就是kevent()操作的最基本的事件结构，在一个 kqueue 中，{ident, filter} 确定一个唯一的事件：
  // 在 kevent 返回时，将读/写缓冲区的可读字节数/可写空间大小告诉应用程序。基于这个特性，使用 kqueue 的应用一般不使用非阻塞IO
  return kqueue(); 
}

/**
 * 等待直到注册的事件发生
 */
int co_epoll_wait(int epfd, struct co_epoll_res* events, int maxevents, int timeout) {
  struct timespec t = {0};
  if (timeout > 0) {
    t.tv_sec = timeout;
  }

  int ret = kevent(epfd, NULL, 0,                // register null
                   events->eventlist, maxevents, // just retrival
                   (-1 == timeout) ? NULL : &t);

  int j = 0;
  for (int i = 0; i < ret; i++) {
    struct kevent& kev = events->eventlist[i];
    struct kevent_pair_t* ptr = (struct kevent_pair_t*) kev.udata;
    struct epoll_event* ev = events->events + i;
    if (0 == ptr->fire_idx) {
      ptr->fire_idx = i + 1;
      memset(ev, 0, sizeof(*ev));
      ++j;
    } else {
      ev = events->events + ptr->fire_idx - 1;
    }
    if (EVFILT_READ == kev.filter) {
      ev->events |= EPOLLIN;
    } else if (EVFILT_WRITE == kev.filter) {
      ev->events |= EPOLLOUT;
    }
    ev->data.u64 = ptr->u64;
  }
  for (int i = 0; i < ret; i++) {
    ((struct kevent_pair_t*) (events->eventlist[i].udata))->fire_idx = 0;
  }
  return j;
}

int co_epoll_del(int epfd, int fd) {
  struct timespec t = {0};
  struct kevent_pair_t* ptr = (struct kevent_pair_t*) get_fd_map()->get(fd);
  if (!ptr)
    return 0;
  if (EPOLLIN & ptr->events) {
    struct kevent kev = {0};
    kev.ident = fd;
    kev.filter = EVFILT_READ;
    kev.flags = EV_DELETE;
    kevent(epfd, &kev, 1, NULL, 0, &t);
  }
  if (EPOLLOUT & ptr->events) {
    struct kevent kev = {0};
    kev.ident = fd;
    kev.filter = EVFILT_WRITE;
    kev.flags = EV_DELETE;
    kevent(epfd, &kev, 1, NULL, 0, &t);
  }
  get_fd_map()->clear(fd);
  free(ptr);
  return 0;
}

int co_epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
  if (EPOLL_CTL_DEL == op) {
    return co_epoll_del(epfd, fd);
  }

  const int flags = (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
  if (ev->events & ~flags) {
    return -1;
  }

  if (EPOLL_CTL_ADD == op && get_fd_map()->get(fd)) {
    errno = EEXIST;
    return -1;
  } else if (EPOLL_CTL_MOD == op && !get_fd_map()->get(fd)) {
    errno = ENOENT;
    return -1;
  }

  struct kevent_pair_t* ptr = (struct kevent_pair_t*) get_fd_map()->get(fd);
  if (!ptr) {
    ptr = (kevent_pair_t*) calloc(1, sizeof(kevent_pair_t));
    get_fd_map()->set(fd, ptr);
  }

  int ret = 0;
  struct timespec t = {0};

  // printf("ptr->events 0x%X\n",ptr->events);

  if (EPOLL_CTL_MOD == op) {
    // 1.delete if exists
    if (ptr->events & EPOLLIN) {
      struct kevent kev = {0};
      EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
      kevent(epfd, &kev, 1, NULL, 0, &t);
    }
    // 1.delete if exists
    if (ptr->events & EPOLLOUT) {
      struct kevent kev = {0};
      EV_SET(&kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
      ret = kevent(epfd, &kev, 1, NULL, 0, &t);
      // printf("delete write ret %d\n",ret );
    }
  }

  do {
    if (ev->events & EPOLLIN) {
      // 2.add
      struct kevent kev = {0};
      EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, ptr);
      ret = kevent(epfd, &kev, 1, NULL, 0, &t);
      if (ret)
        break;
    }
    if (ev->events & EPOLLOUT) {
      // 2.add
      struct kevent kev = {0};
      EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, ptr);
      ret = kevent(epfd, &kev, 1, NULL, 0, &t);
      if (ret)
        break;
    }
  } while (0);

  if (ret) {
    get_fd_map()->clear(fd);
    free(ptr);
    return ret;
  }

  ptr->events = ev->events;
  ptr->u64 = ev->data.u64;

  return ret;
}

struct co_epoll_res* co_epoll_res_alloc(int n) {
  struct co_epoll_res* ptr = (struct co_epoll_res*) malloc(sizeof(struct co_epoll_res));

  ptr->size = n;
  ptr->events = (struct epoll_event*) calloc(1, n * sizeof(struct epoll_event));
  ptr->eventlist = (struct kevent*) calloc(1, n * sizeof(struct kevent));

  return ptr;
}

void co_epoll_res_free(struct co_epoll_res* ptr) {
  if (!ptr)
    return;
  if (ptr->events)
    free(ptr->events);
  if (ptr->eventlist)
    free(ptr->eventlist);
  free(ptr);
}

#endif
