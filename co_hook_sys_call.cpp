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

#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <errno.h>
#include <netinet/in.h>
#include <time.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <resolv.h>

#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_routine_specific.h"
#include <map>
#include <time.h>

typedef long long ll64_t;

// 注释参考: 
// http://kaiyuan.me/2017/05/03/function_wrapper/
// https://blog.csdn.net/breaksoftware/article/details/77340634

// 这个源文件主要功能是hook了libco协程库中使用的Linux系统调用，又原先的系统调用功能，增加了针对协程(coroutine)的部分

// 在研究C++中协程机制时，发现有些实现通过hack掉glibc的read、write等IO操作函数，以达到迁移协程框架时，
// 最小化代码改动，遂小小研究一下Linux下的hook机制。

// 简单来说，就是利用动态链接的原理来修改符号指向，从而达到『偷梁换柱』的编程效果。我们简单地来看一下 libco 
// 是如何使用动态链接 Hook 系统函数的。事实上，libco 最大的特点就是将系统中的关于网络操作的阻塞函数全部进行
// 相应的非侵入式改造，例如对于 read，write 函数，libco 均定义了自己的版本，然后通过 LD_PRELOAD 进行运行
// 时(提前在搜索路径中定义一个同名、同形参、同返回值的目标函数)的解析，从而来达到阻塞时自动让出协程，并在 IO 
// 事件发生时唤醒协程的目的。

// LD_PRELOAD 作用：通过命令 export LD_PRELOAD="库文件路径"，设置要优先替换动态链接库，是个环境变量，用于
// 动态库的加载，动态库加载的优先级最高，一般情况下，其加载顺序为：
// LD_PRELOAD > LD_LIBRARY_PATH > /etc/ld.so.cache > /lib>/usr/lib。

// Linux用户层Hook非常简单。我们只要定义一个和被Hook的API相同名称、参数、返回值的函数即可。

/** 
 * 套接字hook信息结构 - 存储hook函数中跟套接字相关的信息
 *
 * 为什么需要该结构呢? - 举例说明, 套接字的 O_NONBLOCK 属性决定了hook后的read函数是直接调用系统的 read 函数,
 * 还是先向内核注册事件(向内核注册事件时又需要读超时时间), 然后 切换协程 并等待事件发生时 切换回该协程, 最后调用
 * 系统的 read 函数. 如果不把这些信息(是否O_NONBLOCK, 读写超时时间等)传递到被hook函数中, 我们就无法实现hook
 * 函数的逻辑. 这就是需要该结构的原因！
 * 说句题外话, 其实我觉得该结构是非必需的, O_NONBLOCK属性可通过未hook的fcntl函数获得, 超时时间采用全局设置也未尝不可.
 */
struct rpchook_t {
  int user_flag;                // 套接字的阻塞/非阻塞属性(O_NONBLOCK)
  struct sockaddr_in dest;      // 套接字目的主机地址
  int domain;                   // 套接字类型：AF_LOCAL, AF_INET

  struct timeval read_timeout;  // 套接字读超时时间
  struct timeval write_timeout; // 该套接写超时时间
};

/** 
 * GetPid - 获取当前线程id
 * gettid()获取的是内核中的真实线程id, 而pthread_self()获取的是posix线程id, 不一样的，
 * 使用pthread库应该加上编译选项：-lpthread
 * 
 * 1、pthread_self()函数是线程库POSIX Phtread实现函数，它返回的线程ID是由线程库封装过然后返回的。既然是线程库函数，
 * 那么该函数返回的ID也就只在进程中有意义，与操作系统的任务调度之间无法建立有效关联。
 * 2、另外glibc的Pthreads实现实际上把pthread_t用作一个结构体指针(它的类型是unsigned long)，指向一块动态分配的内存，
 * 而且这块内存是反复使用的。这就造成pthread_t的值很容易重复。Pthreads只保证同一进程内，同一时刻的各个线程的id不同；不
 * 能保证同一进程先后多个线程具有不同的id。(当前一个线程结束其生命周期，进程又新创建了一个线程，那么该线程ID可能会使用消
 * 亡线程的ID)。
 * 
 * gettid()函数就是Linux提供的函数，它返回的ID就是"线程"(轻量级进程)ID，相当于内核线程ID。
 */
static inline pid_t GetPid() {
  char** p = (char**) pthread_self();
  return p ? *(pid_t*) (p + 18) : getpid(); // TODO: 这里不懂
}

/**
 * 套接字hook信息数组 - 存储(该线程内)所有协程中的套接字hook信息, 便于套接字hook信息在被hook的系统调用之间传递,
 * 部分被hook函数用这些信息来控制函数逻辑(详见下面被hook的系统调用).
 * 理解这个数组是重点, 一部分被hook的系统调用初始化这些数组中的元素, 另一部分被hook的系统调用获取数组元素来控制函数逻辑.
 */
static rpchook_t* g_rpchook_socket_fd[102400] = {0};

/**
 * 对每个被hook的系统调用声明一种函数指针类型 
 */
typedef int (*socket_pfn_t) (int domain, int type, int protocol);
typedef int (*connect_pfn_t) (int socket, const struct sockaddr* address, socklen_t address_len);
typedef int (*close_pfn_t) (int fd);
typedef ssize_t (*read_pfn_t) (int fildes, void* buf, size_t nbyte);
typedef ssize_t (*write_pfn_t) (int fildes, const void* buf, size_t nbyte);

typedef ssize_t (*sendto_pfn_t) (int socket, const void* message, size_t length,
                                 int flags, const struct sockaddr *dest_addr,
                                 socklen_t dest_len);

typedef ssize_t (*recvfrom_pfn_t) (int socket, void* buffer, size_t length,
                                   int flags, struct sockaddr* address,
                                   socklen_t* address_len);

typedef ssize_t (*send_pfn_t) (int socket, const void* buffer, size_t length, int flags);
typedef ssize_t (*recv_pfn_t) (int socket, void* buffer, size_t length, int flags);

typedef int (*poll_pfn_t) (struct pollfd fds[], nfds_t nfds, int timeout);

typedef int (*setsockopt_pfn_t) (int socket, int level, int option_name,
                                 const void* option_value, socklen_t option_len);

typedef int (*fcntl_pfn_t) (int fildes, int cmd, ...);
typedef struct tm* (*localtime_r_pfn_t) (const time_t *timep, struct tm *result);

typedef void* (*pthread_getspecific_pfn_t) (pthread_key_t key);
typedef int (*pthread_setspecific_pfn_t) (pthread_key_t key, const void* value);

typedef int (*setenv_pfn_t) (const char* name, const char* value, int overwrite);
typedef int (*unsetenv_pfn_t) (const char* name);
typedef char* (*getenv_pfn_t) (const char* name);
typedef hostent* (*gethostbyname_pfn_t) (const char *name);
typedef res_state (*__res_state_pfn_t) ();
typedef int (*__poll_pfn_t) (struct pollfd fds[], nfds_t nfds, int timeout);

/**
 * dlsym()函数是Linux的系统调用，获取共享对象(.so)或可执行文件(.o)中符号的地址，例如：read()/write()/socket()等
 * 
 * 将动态库(.so)中被hook的系统调用的地址(即函数指针)绑定到以g_sys_##name##__func命名的函数指针
 * 为什么要这么做呢? - 这样做的目的是在链接阶段(Link)让系统调用在动态库中找不到对应的实现(使用LD_PRELOAD机制), 而来
 * 链接到我们代码中的同名函数(即被hook的函数)。
 * 
 * 特殊句柄 RTLD_NEXT 允许从调用方链接映射列表中的下一个关联目标文件获取符号。
 * https://docs.oracle.com/cd/E19253-01/819-7050/chapter3-24/index.html
 * RTLD_NEXT，大概意思就是说，传入这个参数，找到的函数指针是后面第一次出现这个函数名的函数指针。
 * 在当前对象之后的搜索顺序中查找下一个所需符号。这允许人们在另一个共享对象中提供一个函数的包装器，因此，例如，
 * 预加载的共享对象中的函数定义（参见ld.so（8）中的LD_PRELOAD）可以找到并调用在另一个共享对象中提供的“真实”函数
 * （或者就此而言，在存在多个预加载层的情况下，函数的“下一个”定义）。
 */
static socket_pfn_t g_sys_socket_func = (socket_pfn_t) dlsym(RTLD_NEXT, "socket");
static connect_pfn_t g_sys_connect_func = (connect_pfn_t) dlsym(RTLD_NEXT, "connect");
static close_pfn_t g_sys_close_func = (close_pfn_t) dlsym(RTLD_NEXT, "close");

static read_pfn_t g_sys_read_func = (read_pfn_t) dlsym(RTLD_NEXT, "read");
static write_pfn_t g_sys_write_func = (write_pfn_t) dlsym(RTLD_NEXT, "write");

static sendto_pfn_t g_sys_sendto_func = (sendto_pfn_t) dlsym(RTLD_NEXT, "sendto");
static recvfrom_pfn_t g_sys_recvfrom_func = (recvfrom_pfn_t) dlsym(RTLD_NEXT, "recvfrom");

static send_pfn_t g_sys_send_func = (send_pfn_t) dlsym(RTLD_NEXT, "send");
static recv_pfn_t g_sys_recv_func = (recv_pfn_t) dlsym(RTLD_NEXT, "recv");

static poll_pfn_t g_sys_poll_func = (poll_pfn_t) dlsym(RTLD_NEXT, "poll");

static setsockopt_pfn_t g_sys_setsockopt_func = (setsockopt_pfn_t) dlsym(RTLD_NEXT, "setsockopt");
static fcntl_pfn_t g_sys_fcntl_func = (fcntl_pfn_t) dlsym(RTLD_NEXT, "fcntl");

static setenv_pfn_t g_sys_setenv_func = (setenv_pfn_t) dlsym(RTLD_NEXT, "setenv");
static unsetenv_pfn_t g_sys_unsetenv_func = (unsetenv_pfn_t) dlsym(RTLD_NEXT, "unsetenv");
static getenv_pfn_t g_sys_getenv_func = (getenv_pfn_t) dlsym(RTLD_NEXT, "getenv");
static __res_state_pfn_t g_sys___res_state_func = (__res_state_pfn_t) dlsym(RTLD_NEXT, "__res_state");

static gethostbyname_pfn_t g_sys_gethostbyname_func = (gethostbyname_pfn_t) dlsym(RTLD_NEXT, "gethostbyname");

static __poll_pfn_t g_sys___poll_func = (__poll_pfn_t) dlsym(RTLD_NEXT, "__poll");

/*
static pthread_getspecific_pfn_t g_sys_pthread_getspecific_func
                        =
(pthread_getspecific_pfn_t)dlsym(RTLD_NEXT,"pthread_getspecific");

static pthread_setspecific_pfn_t g_sys_pthread_setspecific_func
                        =
(pthread_setspecific_pfn_t)dlsym(RTLD_NEXT,"pthread_setspecific");

static pthread_rwlock_rdlock_pfn_t g_sys_pthread_rwlock_rdlock_func
                        =
(pthread_rwlock_rdlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_rdlock");

static pthread_rwlock_wrlock_pfn_t g_sys_pthread_rwlock_wrlock_func
                        =
(pthread_rwlock_wrlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_wrlock");

static pthread_rwlock_unlock_pfn_t g_sys_pthread_rwlock_unlock_func
                        =
(pthread_rwlock_unlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_unlock");
*/

static inline unsigned long long get_tick_count() { // 该函数未被使用
  uint32_t lo, hi;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi));
  return ((unsigned long long) lo) | (((unsigned long long) hi) << 32);
}

struct rpchook_connagent_head_t { // 未使用该结构
  unsigned char bVersion;
  struct in_addr iIP;
  unsigned short hPort;
  unsigned int iBodyLen;
  unsigned int iOssAttrID;
  unsigned char bIsRespNotExist;
  unsigned char sReserved[6];
} __attribute__((packed)); // 按1字节压缩

/** 
 * hook系统调用 - 将动态库中名为name的系统调用地址(即函数指针)绑定到以g_sys_##name##__func命名的函数指针 
 */
#define HOOK_SYS_FUNC(name)                                                    \
  if (!g_sys_##name##_func) {                                                  \
    g_sys_##name##_func = (name##_pfn_t)dlsym(RTLD_NEXT, #name);               \
  }

/**
 * diff_ms - 计算以毫秒为单位的时间差
 * @param begin - (input) 开始时间
 * @param end - (input) 结束时间
 * @return 毫秒为单位的时间差
 */
static inline ll64_t diff_ms(struct timeval& begin, struct timeval& end) {
  ll64_t u = (end.tv_sec - begin.tv_sec);
  u *= 1000 * 10;
  u += (end.tv_usec - begin.tv_usec) / (100);
  return u;
}

/**
 * get_by_fd - 在套接字hook信息数组(g_rpchook_socket_fd)中获取套接字fd对应的 rpchook_t 类型变量的指针
 * @param fd - (input) 套接字文件描述符
 * @return 成功返回 rpchook_t 类型变量的指针, 失败返回NULL
 */
static inline rpchook_t* get_by_fd(int fd) {
  if (fd > -1 && fd < (int) sizeof(g_rpchook_socket_fd) / (int) sizeof(g_rpchook_socket_fd[0])) {
    return g_rpchook_socket_fd[fd];
  }
  return NULL;
}

/**
 * alloc_by_fd - 为套接字fd分配对应的 rpchook_t 类型类型的存储空间, 并将存储空间的地址加入到套接字hook信息数组(g_rpchook_socket_fd)中
 * @param fd - (input) 套接字文件描述符
 * @return 成功返回rpchook_t类型变量的指针, 失败返回NULL
 */
static inline rpchook_t* alloc_by_fd(int fd) {
  if (fd > -1 && fd < (int) sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0])) {
    rpchook_t* lp = (rpchook_t*) calloc(1, sizeof(rpchook_t));
    lp->read_timeout.tv_sec = 1;
    lp->write_timeout.tv_sec = 1;
    g_rpchook_socket_fd[fd] = lp;
    return lp;
  }
  return NULL;
}

/**
 * free_by_fd - 在套接字hook信息数组(g_rpchook_socket_fd)中释放套接字fd对应rpchook_t类型变量的存储空间
 * @param fd - (input) 套接字文件描述符
 * @return
 */
static inline void free_by_fd(int fd) {
  if (fd > -1 && fd < (int) sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0])) {
    rpchook_t* lp = g_rpchook_socket_fd[fd];
    if (lp) {
      g_rpchook_socket_fd[fd] = NULL;
      free(lp);
    }
  }
  return;
}

/**
 * 系统函数hook原理(以socket()为例，其他类似)：
 * 该函数必须与libc.so(glibc.so)中的的socket函数完全一致(名称、参数列表、返回值类型)，这样才会在业务中调用socket()时，
 * 执行这里，因为这时dlsym默认句柄(handler)是RTLD_DEFAULT，按照路径会首先索引到当前.so动态库中来执行socket()，在该函
 * 数中，我们通过dlsym(RTLD_NEXT，"socket")来寻找下一个(即libc.so中的)socket函数符号，来获取原始的函数地址，进行执行。
 * 
 * socket - 被hook后的socket函数, 主要是为套接字fd分配对应的 rpchook_t 类型的内存空间, 并往 g_rpchook_socket_fd 
 * 中添加该内存空间的地址(指针指向的变量未全部初始化) 
 */
int socket(int domain, int type, int protocol) {
  // 重命名动态库中的socket系统调用
  HOOK_SYS_FUNC(socket);

  // 协程禁止hook系统调用, 则直接调用socket系统调用返回socket文件描述符fd
  if (!co_is_enable_sys_hook()) {
    return g_sys_socket_func(domain, type, protocol);
  }

  // 协程使用了hook, 则直接调用socket系统调用获取socket文件描述符fd
  int fd = g_sys_socket_func(domain, type, protocol); // 这里执行的是原始(libc.so中)的socket()
  if (fd < 0) {
    return fd;
  }

  // 为fd分配 rpchook_t 类型的内存空间, 其中存储套接字hook信息, 并将其加入套接字hook信息数组 g_rpchook_socket_fd 中
  rpchook_t* lp = alloc_by_fd(fd);
  lp->domain = domain;

  // 设置套接字fd属性：该fd设置为 NONBLOCK(非阻塞)
  fcntl(fd, F_SETFL, g_sys_fcntl_func(fd, F_GETFL, 0));

  return fd;
}

/** 
 * co_accpet - accpet系统调用的封装而已
 */
int co_accept(int fd, struct sockaddr* addr, socklen_t* len) {
  int cli = accept(fd, addr, len);
  if (cli < 0) {
    return cli;
  }
  alloc_by_fd(cli);
  return cli;
}

/** 
 * connect - 被hook后的connect函数, 主要是初始化(g_rpchook_socket_fd中)套接字fd对应的rpchook_t类型变量的dest成员
 */
int connect(int fd, const struct sockaddr* address, socklen_t address_len) {
  HOOK_SYS_FUNC(connect);

  if (!co_is_enable_sys_hook()) { // 没有开启系统api hook机制
    return g_sys_connect_func(fd, address, address_len);
  }

  // 1.sys call(系统原始调用)，之前socket阶段已经对该fd设置了NONBLOCK状态标志了
  int ret = g_sys_connect_func(fd, address, address_len);

  rpchook_t* lp = get_by_fd(fd);
  if (!lp)
    return ret;

  if (sizeof(lp->dest) >= address_len) { // 把目的地址存入缓存
    memcpy(&(lp->dest), address, (int) address_len); // /tmp/connagent_unix_domain_socket
  }
  if (O_NONBLOCK & lp->user_flag) {
    return ret; // 非阻塞状态的连接，这里返回了
  }

  if (!(ret < 0 && errno == EINPROGRESS)) {
    return ret;
  }

  // 2.wait
  int pollret = 0;
  struct pollfd pf = {0};
  for (int i = 0; i < 3; i++) { // 25s * 3 = 75s
    memset(&pf, 0, sizeof(pf));
    pf.fd = fd; // 需要轮询的文件描述符
    // 等待发生的事件：写数据不会导致阻塞 | 指定的文件描述符发生错误 | 指定的文件描述符挂起事件
    pf.events = (POLLOUT | POLLERR | POLLHUP); 
    pollret = poll(&pf, 1, 25000); // 监听该fd的各个事件，超时时25s
    if (pollret == 1) { // 返回值为1表示该fd已经有事件发生了
      break;
    }
  }

  // 该fd可以写数据了
  if (pf.revents & POLLOUT) { // connect succ
    // 3.check getsockopt ret
    int err = 0;
    socklen_t errlen = sizeof(err);
    /**
     * 参数：
     * socket：将要被设置或者获取选项的套接字
     * level：选项所在的协议层
     * optname：需要访问的选项名
     * optval：对于getsockopt()，指向返回选项值的缓存。
     *         对于setsockopt()，指向包含新选项值的缓存
     * optlen：对于getsockopt()，作为入口参数时，选项值的最大长度。作为出口参数时，选项值的实际长度
     *         对于setsockopt()，The size, in bytes, of the optval buffer
     * level指定控制套接字的层次.可以取三种值: 
     * 1) SOL_SOCKET:通用套接字选项
     * 2) IPPROTO_IP:IP选项
     * 3) IPPROTO_TCP:TCP选项
     * optname指定控制的方式(选项的名称)，我们下面详细解释
     * optval获得或者是设置套接字选项.根据选项名称的数据类型进行转换
     * 详见：https://www.cnblogs.com/ranjiewen/p/5716343.html
     * SO_ERROR: 获得套接字错误
     */
    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (ret < 0) {
      return ret;
    } else if (err != 0) {
      errno = err;
      return -1;
    }
    errno = 0;
    return 0;
  }

  errno = ETIMEDOUT;
  return ret;
}

/**
 * close - 被hook后的close函数, 主要是释放(g_rpchook_socket_fd中)
 * 套接字fd对应的rpchook_t类型存储空间 
 */
int close(int fd) {
  HOOK_SYS_FUNC(close);

  // 协程禁止hook系统调用, 则直接调用系统调用
  if (!co_is_enable_sys_hook()) {
    return g_sys_close_func(fd);
  }

  // 协程hook系统调用, 则释放(g_rpchook_socket_fd中)套接字fd对应的rpchook_t类型的存储空间
  free_by_fd(fd);
  int ret = g_sys_close_func(fd);

  return ret;
}

/**
 * read - 被hook后的read函数, 主要是向内核注册套接字fd上的事件 
 */
ssize_t read(int fd, void* buf, size_t nbyte) {
  HOOK_SYS_FUNC(read);

  // 协程禁止hook系统调用, 则直接调用系统原生read()
  if (!co_is_enable_sys_hook()) {
    return g_sys_read_func(fd, buf, nbyte);
  }

  // 协程hook系统调用，根据套接字是否为非阻塞(NON_BLOCK)选择不同的处理方式
  rpchook_t* lp = get_by_fd(fd);

  if (!lp || (O_NONBLOCK & lp->user_flag)) { // 非阻塞, 直接调用系统调用
    ssize_t ret = g_sys_read_func(fd, buf, nbyte);
    return ret;
  }
  int timeout = (lp->read_timeout.tv_sec * 1000) + (lp->read_timeout.tv_usec / 1000);

	// 阻塞, 向内核注册套接字fd的事件
	// poll如果未hook，则直接调用poll系统调用;
	// poll如果被hook，则调用co_poll向内核注册, co_poll中会切换协程, 协程被恢复时将会从co_poll中的挂起点继续运行
  struct pollfd pf = {0};
  pf.fd = fd;
  // 等待发生的事件：有数据可读 | 指定的文件描述符发生错误 | 指定的文件描述符挂起事件
  pf.events = (POLLIN | POLLERR | POLLHUP);
  int pollret = poll(&pf, 1, timeout);

  ssize_t readret = g_sys_read_func(fd, (char*) buf, nbyte); // 调用系统原始read()

  if (readret < 0) {
    co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",  
        fd, readret, errno, pollret, timeout);
  }

  return readret;
}

/**
 * write - 被hook后的write函数, 主要是向内核注册套接字fd上的事件
 */
ssize_t write(int fd, const void* buf, size_t nbyte) {
  HOOK_SYS_FUNC(write);

  // 协程禁止hook系统调用, 则直接调用系统调用
  if (!co_is_enable_sys_hook()) {
    return g_sys_write_func(fd, buf, nbyte);
  }

  rpchook_t* lp = get_by_fd(fd);

  // 协程hook系统调用, 根据套接字是否为非阻塞选择不同的处理方式
  if (!lp || (O_NONBLOCK & lp->user_flag)) { // 非阻塞, 直接调用系统调用
    ssize_t ret = g_sys_write_func(fd, buf, nbyte);
    return ret;
  }

  // 阻塞, 向内核注册套接字fd的事件
	// poll如果未hook，则直接调用poll系统调用;
	// poll如果被hook，则调用co_poll向内核注册, co_poll中会切换协程, 协程被恢复时将会从co_poll中的挂起点继续运行
  size_t wrotelen = 0;
  int timeout = (lp->write_timeout.tv_sec * 1000) + (lp->write_timeout.tv_usec / 1000);
  ssize_t writeret = g_sys_write_func(fd, (const char*) buf + wrotelen, nbyte - wrotelen);
  if (writeret == 0) {
    return writeret;
  }
  if (writeret > 0) {
    wrotelen += writeret;
  }

  while (wrotelen < nbyte) {
    // buf中的数据未全部写到fd上, 则向内核注册套接字fd的事件
    struct pollfd pf = {0};
    pf.fd = fd;
    // 
    pf.events = (POLLOUT | POLLERR | POLLHUP);
    poll(&pf, 1, timeout);

    writeret = g_sys_write_func(fd, (const char*) buf + wrotelen, nbyte - wrotelen);

    if (writeret <= 0) {
      break;
    }
    wrotelen += writeret;
  }
  if (writeret <= 0 && wrotelen == 0) {
    return writeret;
  }
  return wrotelen;
}

/**
 * sendto - 被hook后的sendto函数, 主要是向内核注册套接字fd上的事件
 */
ssize_t sendto(int socket, const void* message, size_t length, int flags,
               const struct sockaddr* dest_addr, socklen_t dest_len) {
  /**
   * 1.no enable sys call ? sys
   * 2.( !lp || lp is non block ) ? sys
   * 3.try
   * 4.wait
   * 5.try
   */
  HOOK_SYS_FUNC(sendto);

  if (!co_is_enable_sys_hook()) {
    return g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  }

  rpchook_t* lp = get_by_fd(socket);
  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  }

  ssize_t ret = g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  if (ret < 0 && EAGAIN == errno) {
    int timeout = (lp->write_timeout.tv_sec * 1000) + (lp->write_timeout.tv_usec / 1000);

    struct pollfd pf = {0};
    pf.fd = socket;
    pf.events = (POLLOUT | POLLERR | POLLHUP);
    poll(&pf, 1, timeout);

    ret = g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  }
  return ret;
}

/**
 * recvfrom - 被hook后的recvfrom函数, 主要是向内核注册套接字fd上的事件
 */
ssize_t recvfrom(int socket, void* buffer, size_t length, int flags,
                 struct sockaddr* address, socklen_t* address_len) {

  HOOK_SYS_FUNC(recvfrom);
  if (!co_is_enable_sys_hook()) {
    return g_sys_recvfrom_func(socket, buffer, length, flags, address, address_len);
  }

  rpchook_t* lp = get_by_fd(socket);
  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_recvfrom_func(socket, buffer, length, flags, address, address_len);
  }

  int timeout = (lp->read_timeout.tv_sec * 1000) + (lp->read_timeout.tv_usec / 1000);

  struct pollfd pf = {0};
  pf.fd = socket;
  pf.events = (POLLIN | POLLERR | POLLHUP);
  poll(&pf, 1, timeout);

  ssize_t ret = g_sys_recvfrom_func(socket, buffer, length, flags, address, address_len);
  return ret;
}

/**
 * send - 被hook后的send函数, 主要是向内核注册套接字fd上的事件
 */
ssize_t send(int socket, const void* buffer, size_t length, int flags) {
  HOOK_SYS_FUNC(send);

  if (!co_is_enable_sys_hook()) {
    return g_sys_send_func(socket, buffer, length, flags);
  }
  rpchook_t* lp = get_by_fd(socket);

  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_send_func(socket, buffer, length, flags);
  }

  ssize_t writeret = g_sys_send_func(socket, buffer, length, flags);
  if (writeret == 0) { // 没数据可以发送
    return writeret;
  }

  size_t wrotelen = 0; // 总共发送了多少(wrotelen)数据
  if (writeret > 0) {
    wrotelen += writeret;
  }
  int timeout = (lp->write_timeout.tv_sec * 1000) + (lp->write_timeout.tv_usec / 1000);
  while (wrotelen < length) { // 循环发送完所有数据
    struct pollfd pf = {0};
    pf.fd = socket;
    pf.events = (POLLOUT | POLLERR | POLLHUP);
    poll(&pf, 1, timeout);

    writeret = g_sys_send_func(socket, (const char *)buffer + wrotelen, length - wrotelen, flags);

    if (writeret <= 0) {
      break; // 数据发完了
    }
    wrotelen += writeret;
  }
  if (writeret <= 0 && wrotelen == 0) {
    return writeret;
  }
  return wrotelen;
}

/**
 * recv - 被hook后的recv函数, 主要是向内核注册套接字fd上的事件
 */
ssize_t recv(int socket, void* buffer, size_t length, int flags) {
  HOOK_SYS_FUNC(recv);

  if (!co_is_enable_sys_hook()) {
    return g_sys_recv_func(socket, buffer, length, flags);
  }
  rpchook_t* lp = get_by_fd(socket);

  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_recv_func(socket, buffer, length, flags);
  }
  int timeout = (lp->read_timeout.tv_sec * 1000) + (lp->read_timeout.tv_usec / 1000);

  struct pollfd pf = {0};
  pf.fd = socket;
  pf.events = (POLLIN | POLLERR | POLLHUP);

  int pollret = poll(&pf, 1, timeout);

  ssize_t readret = g_sys_recv_func(socket, buffer, length, flags);

  if (readret < 0) {
    co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
               socket, readret, errno, pollret, timeout);
  }

  return readret;
}

/**
 * co_poll_inner()向内核注册, co_poll_inner()中会切换协程, 协程被恢复时将会从co_poll_inner()中的挂起点继续运行
 */
extern int co_poll_inner(stCoEpoll_t* ctx, 
                         struct pollfd fds[], nfds_t nfds, 
                         int timeout, poll_pfn_t pollfunc);

/**
 * 成功时，poll()返回结构体中revents域不为0的文件描述符个数；
 * 如果在超时前没有任何事件发生，poll()返回0；
 * 失败时，poll()返回-1，并设置errno为下列值之一：
 * EBADF        一个或多个结构体中指定的文件描述符无效。
 * EFAULTfds    指针指向的地址超出进程的地址空间。
 * EINTR        请求的事件之前产生一个信号，调用可以重新发起。
 * EINVALnfds   参数超出PLIMIT_NOFILE值。
 * ENOMEM　　     可用内存不足，无法完成请求。
 * 
 * @param timeout timeout参数指定等待的毫秒数，无论I/O是否准备好，poll都会返回。
 * timeout指定为负数值表示无限超时，使poll()一直挂起直到一个指定事件发生；
 * timeout为0指示poll调用立即返回并列出准备好I/O的文件描述符，但并不等待其它的事件。
 * 这种情况下，poll()就像它的名字那样，一旦选举出来，立即返回
 */
int poll(struct pollfd fds[], nfds_t nfds, int timeout) {
  HOOK_SYS_FUNC(poll);

  if (!co_is_enable_sys_hook() || timeout == 0) {
    return g_sys_poll_func(fds, nfds, timeout);
  }

  pollfd* fds_merge = NULL;
  nfds_t nfds_merge = 0;
  std::map<int, int> m; // fd --> idx
  std::map<int, int>::iterator it;
  if (nfds > 1) {
    fds_merge = (pollfd*) malloc(sizeof(pollfd) * nfds);
    for (size_t i = 0; i < nfds; i++) { // 可能有重复的fd，需要合并(合并其epoll监听的事件)
      if ((it = m.find(fds[i].fd)) == m.end()) { // map中该fd miss了
        fds_merge[nfds_merge] = fds[i];
        m[fds[i].fd] = nfds_merge; // key:fd,val:fds_merge中fd的index(索引)
        nfds_merge++;
      } else { // map中该fd hit(命中)
        int j = it->second; // 在fds_merge中的index
        fds_merge[j].events |= fds[i].events; // merge in j slot
      }
    }
  }

  int ret = 0;
  if (nfds_merge == nfds || nfds == 1) {
    ret = co_poll_inner(co_get_epoll_ct(), fds, nfds, timeout, g_sys_poll_func);
  } else {
    ret = co_poll_inner(co_get_epoll_ct(), fds_merge, nfds_merge, timeout, g_sys_poll_func);
    if (ret > 0) {
      for (size_t i = 0; i < nfds; i++) {
        it = m.find(fds[i].fd);
        if (it != m.end()) {
          int j = it->second;
          // revents字段是真实发生的事件集合；events字段是需要监听的事件集合
          // 这里再过滤一次fd真实发生的事件
          fds[i].revents = fds_merge[j].revents & fds[i].events;
        }
      }
    }
  }
  free(fds_merge);
  return ret;
}

/**
 * setsockopt - 被hook后的setsockopt函数, 主要是初始化(g_rpchook_socket_fd中)套接字fd
 * 对应的rpchook_t类型变量的read_timeout和write_timeout成员
 */
int setsockopt(int fd, int level, int option_name, const void* option_value, socklen_t option_len) {
  HOOK_SYS_FUNC(setsockopt);

  if (!co_is_enable_sys_hook()) {
    return g_sys_setsockopt_func(fd, level, option_name, option_value, option_len);
  }
  rpchook_t* lp = get_by_fd(fd);

  if (lp && SOL_SOCKET == level) { // 通用套接字选项
    struct timeval* val = (struct timeval*) option_value;
    if (SO_RCVTIMEO == option_name) {
      memcpy(&lp->read_timeout, val, sizeof(*val));
    } else if (SO_SNDTIMEO == option_name) {
      memcpy(&lp->write_timeout, val, sizeof(*val));
    }
  }
  return g_sys_setsockopt_func(fd, level, option_name, option_value, option_len);
}

/** 
 * fcntl - 被hook后的fcntl函数, 主要是初始化(g_rpchook_socket_fd中)套接字fd对应的rpchook_t类型变量的user_flag成员
 */
int fcntl(int fildes, int cmd, ...) {
  HOOK_SYS_FUNC(fcntl);

  if (fildes < 0) {
    return __LINE__;
  }

  va_list arg_list;
  va_start(arg_list, cmd); // arg_list指向第一个可变参数列表(...)中的第1个可变参数

  int ret = -1;
  rpchook_t* lp = get_by_fd(fildes);
  switch (cmd) {
  case F_DUPFD: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  
  case F_GETFD: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
  
  case F_SETFD: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }

  case F_GETFL: {
    ret = g_sys_fcntl_func(fildes, cmd);
    if (lp && !(lp->user_flag & O_NONBLOCK)) {
      ret = ret & (~O_NONBLOCK);
    }
  
    break;
  }
  
  case F_SETFL: {
    int param = va_arg(arg_list, int);
    int flag = param;
    if (co_is_enable_sys_hook() && lp) {
      flag |= O_NONBLOCK; // 非阻塞
    }
    ret = g_sys_fcntl_func(fildes, cmd, flag); // 这里设置文件描述符(fd)为 NONBLOCK
    if (0 == ret && lp) {
      lp->user_flag = param; // 这里保持旧的用户参数
    }
  
    break;
  }

  case F_GETOWN: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
  
  case F_SETOWN: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }

  case F_GETLK: {
    struct flock* param = va_arg(arg_list, struct flock*);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  
  case F_SETLK: {
    struct flock* param = va_arg(arg_list, struct flock*);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }

  case F_SETLKW: {
    struct flock* param = va_arg(arg_list, struct flock*);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }

  }

  va_end(arg_list);

  return ret;
}

/**
 * 协程(coroutine)的一个环境变量
 */
struct stCoSysEnv_t {
  char* name;
  char* value;
};

/**
 * 协程(coroutine)的所有环境变量
 */
struct stCoSysEnvArr_t {
  stCoSysEnv_t* data;
  size_t cnt;
};

/**
 * 深度copy全局环境变量
 */
static stCoSysEnvArr_t* dup_co_sysenv_arr(stCoSysEnvArr_t* arr) {
  stCoSysEnvArr_t* lp = (stCoSysEnvArr_t*) calloc(sizeof(stCoSysEnvArr_t), 1);
  if (arr->cnt) {
    lp->data = (stCoSysEnv_t*) calloc(sizeof(stCoSysEnv_t) * arr->cnt, 1);
    lp->cnt = arr->cnt;
    memcpy(lp->data, arr->data, sizeof(stCoSysEnv_t) * arr->cnt);
  }
  return lp;
}

static int co_sysenv_comp(const void* left, const void* right) {
  return strcmp(((stCoSysEnv_t*) left)->name, ((stCoSysEnv_t*) right)->name);
}

static stCoSysEnvArr_t g_co_sysenv = {0}; // 协程系统环境变量

void co_set_env_list(const char* name[], size_t cnt) {
  if (g_co_sysenv.data) { // 已经有数据了，不再重复设置
    return;
  }
  g_co_sysenv.data = (stCoSysEnv_t*) calloc(1, sizeof(stCoSysEnv_t) * cnt);

  for (size_t i = 0; i < cnt; i++) {
    if (name[i] && name[i][0]) {
      g_co_sysenv.data[g_co_sysenv.cnt++].name = strdup(name[i]);
    }
  }
  if (g_co_sysenv.cnt > 1) {
    qsort(g_co_sysenv.data, g_co_sysenv.cnt, sizeof(stCoSysEnv_t), co_sysenv_comp);
    stCoSysEnv_t* lp = g_co_sysenv.data;
    stCoSysEnv_t* lq = g_co_sysenv.data + 1;
    for (size_t i = 1; i < g_co_sysenv.cnt; i++) {
      if (strcmp(lp->name, lq->name)) {
        ++lp;
        if (lq != lp) {
          *lp = *lq;
        }
      }
      ++lq;
    }
    g_co_sysenv.cnt = lp - g_co_sysenv.data + 1;
  }
}

int setenv(const char* n, const char* value, int overwrite) {
  HOOK_SYS_FUNC(setenv)

  if (co_is_enable_sys_hook() && g_co_sysenv.data) {
    stCoRoutine_t* self = co_self(); // 获取当前正在运行的协程实例
    if (self) {
      if (!self->pvEnv) {
        self->pvEnv = dup_co_sysenv_arr(&g_co_sysenv);
      }
      stCoSysEnvArr_t* arr = (stCoSysEnvArr_t*) (self->pvEnv);

      stCoSysEnv_t name = {(char*) n, 0}; // C++11的初始化列表语法

      stCoSysEnv_t* e = (stCoSysEnv_t*) bsearch(&name, arr->data, arr->cnt, sizeof(name), co_sysenv_comp);

      if (e) {
        if (overwrite || !e->value) {
          if (e->value) {
            free(e->value);
          }
          e->value = (value ? strdup(value) : 0);
        }
        return 0;
      }
    }
  }
  return g_sys_setenv_func(n, value, overwrite);
}

int unsetenv(const char* n) {
  HOOK_SYS_FUNC(unsetenv)

  if (co_is_enable_sys_hook() && g_co_sysenv.data) {
    stCoRoutine_t* self = co_self();
    if (self) {
      if (!self->pvEnv) {
        self->pvEnv = dup_co_sysenv_arr(&g_co_sysenv);
      }
      stCoSysEnvArr_t* arr = (stCoSysEnvArr_t*) (self->pvEnv);

      stCoSysEnv_t name = {(char*) n, 0};

      stCoSysEnv_t* e = (stCoSysEnv_t*) bsearch(&name, arr->data, arr->cnt, sizeof(name), co_sysenv_comp);

      if (e) {
        if (e->value) {
          free(e->value);
          e->value = 0;
        }
        return 0;
      }
    }
  }
  return g_sys_unsetenv_func(n);
}

char* getenv(const char* n) {
  HOOK_SYS_FUNC(getenv)

  if (co_is_enable_sys_hook() && g_co_sysenv.data) {
    stCoRoutine_t* self = co_self();

    stCoSysEnv_t name = {(char*) n, 0};

    if (!self->pvEnv) {
      self->pvEnv = dup_co_sysenv_arr(&g_co_sysenv);
    }
    stCoSysEnvArr_t* arr = (stCoSysEnvArr_t*) (self->pvEnv);

    stCoSysEnv_t* e = (stCoSysEnv_t*) bsearch(&name, arr->data, arr->cnt, sizeof(name), co_sysenv_comp);

    if (e) {
      return e->value;
    }
  }
  return g_sys_getenv_func(n);
}

struct hostent* co_gethostbyname(const char* name);

/**
 * 通过域名获取ip地址
 */
struct hostent* gethostbyname(const char* name) {
  HOOK_SYS_FUNC(gethostbyname);

#if defined(__APPLE__) || defined(__FreeBSD__)
  return g_sys_gethostbyname_func(name);
#else
  if (!co_is_enable_sys_hook()) {
    return g_sys_gethostbyname_func(name);
  }
  return co_gethostbyname(name);
#endif
}

struct res_state_wrap {
  struct __res_state state;
};

CO_ROUTINE_SPECIFIC(res_state_wrap, __co_state_wrap);
// ----------------------------------------------------------------------------上面的宏定义展开
  // static pthread_once_t _routine_once_res_state_wrap = PTHREAD_ONCE_INIT;              
  // static pthread_key_t _routine_key_res_state_wrap;                                    
  // static int _routine_init_res_state_wrap = 0;                                         
  // static void _routine_make_key_res_state_wrap() {                                     
  //   (void) pthread_key_create(&_routine_key_res_state_wrap, NULL);                     
  // }                                                                            
  // template <typename T> 
  // class clsRoutineData_routine_res_state_wrap {                  
  // public:                                                                     
  //   inline T* operator->() {                                                  
  //     if (!_routine_init_res_state_wrap) {                                            
  //       pthread_once(&_routine_once_res_state_wrap, _routine_make_key_res_state_wrap);        
  //       _routine_init_res_state_wrap = 1;
  //     }
  //     T* p = (T*) co_getspecific(_routine_key_res_state_wrap);
  //     if (!p) {
  //       p = (T*) calloc(1, sizeof(T));
  //       int ret = co_setspecific(_routine_key_res_state_wrap, p);
  //       if (ret) {
  //         if (p) {
  //           free(p);
  //           p = NULL;
  //         }
  //       }
  //     }
  //     return p;
  //   }
  // };
  // static clsRoutineData_routine_res_state_wrap<res_state_wrap> __co_state_wrap;
// ----------------------------------------------------------------------------上面的宏定义展开

extern "C" {

res_state __res_state() {
  HOOK_SYS_FUNC(__res_state);
  
  if (!co_is_enable_sys_hook()) {
    return g_sys___res_state_func();
  }
  return &(__co_state_wrap->state);
}

int __poll(struct pollfd fds[], nfds_t nfds, int timeout) {
  return poll(fds, nfds, timeout);
}

}

struct hostbuf_wrap {
  struct hostent host;
  char* buffer;
  size_t iBufferSize;
  int host_errno;
};

/**
 * 定义协程私有变量：hostbuf_wrap __co_hostbuf_wrap
 */
CO_ROUTINE_SPECIFIC(hostbuf_wrap, __co_hostbuf_wrap);

/**
 * 通过域名获取ip地址
 */
#if !defined(__APPLE__) && !defined(__FreeBSD__)
struct hostent* co_gethostbyname(const char* name) {
  if (!name) {
    return NULL;
  }

  if (__co_hostbuf_wrap->buffer && __co_hostbuf_wrap->iBufferSize > 1024) {
    free(__co_hostbuf_wrap->buffer);
    __co_hostbuf_wrap->buffer = NULL;
  }
  if (!__co_hostbuf_wrap->buffer) {
    __co_hostbuf_wrap->buffer = (char*) malloc(1024);
    __co_hostbuf_wrap->iBufferSize = 1024;
  }

  struct hostent* host = &__co_hostbuf_wrap->host;
  struct hostent* result = NULL;
  int* h_errnop = &(__co_hostbuf_wrap->host_errno);

  int ret = -1;
  while (ret = gethostbyname_r(name, host, __co_hostbuf_wrap->buffer,
                               __co_hostbuf_wrap->iBufferSize, &result,
                               h_errnop) == ERANGE && *h_errnop == NETDB_INTERNAL) {

    free(__co_hostbuf_wrap->buffer);
    __co_hostbuf_wrap->iBufferSize = __co_hostbuf_wrap->iBufferSize * 2;
    __co_hostbuf_wrap->buffer = (char *)malloc(__co_hostbuf_wrap->iBufferSize);
  }

  if (ret == 0 && (host == result)) {
    return host;
  }
  return NULL;
}
#endif

/**
 * co_enable_hook_sys - 设置当前线程中正在运行的协程中使用hook系统调用
 * 开启系统 api(glbc/libc) hook
 */ 
void co_enable_hook_sys() { // 这函数必须写在这里,否则本文件会被忽略!!!
  stCoRoutine_t* co = GetCurrThreadCo();
  if (co) {
    co->cEnableSysHook = 1;
  }
}
