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

#include "co_routine.h"
#include "co_epoll.h"
#include "co_routine_inner.h"

#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include <errno.h>
#include <poll.h>
#include <sys/time.h>

#include <assert.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

// 腾讯的libco使用了hook技术，做到了在遇到阻塞IO时自动切换协程，（由事件循环co_eventloop检测的）阻塞IO完成时恢复协程，
// 简化异步回调为相对同步方式的功能。其没有使用显示的调度器来管理所有协程（保存协程的相关数据），在协程切换及恢复之间主要
// 依靠epoll_event.data.ptr来传递恢复协程所需的数据。

extern "C" {
  extern void coctx_swap(coctx_t*, coctx_t*) asm("coctx_swap");
};

using namespace std;

stCoRoutine_t* GetCurrCo(stCoRoutineEnv_t* env);
struct stCoEpoll_t;

/**
 * 协程环境类型 - 每个线程有且仅有一个该类型的变量 【相当于当前线程的调度器 marked by habbyge】
 *
 * 该结构的作用是什么呢？ - 我们知道, 非对称协程允许嵌套创建子协程, 为了记录这种嵌套创建的协程, 以便子协程退出
 * 时正确恢复到挂起点(挂起点位于父协程中), 我们就需要记录这种嵌套调用过程; 另外, 协程中的套接字向内核注册了事件,
 * 我们必须保存套接字和协程的对应关系, 以便该线程的eventloop中检测到套接字上事件发生时, 能够恢复该套接字对应的
 * 协程来处理事件.
 * 
 * 其中 pCallStack[0]存储的是主协程，pCallStack[1]存储的是当前正在运行的协程，在生产者与消息费者的示例中，
 * pCallStack 只会使用到前两个数组，对于挂起的协程环境是存储在事件的双向链表中，都过事件触发 机制来控制;
 * pEpoll 表示 EPOLL IO 管理器，是结合定时器或IO事件来管理协程的调度的，这里使用kqueue()来初始化的。
 */
// 代表一个thread中的所有协程，一个线程实例的上下文
struct stCoRoutineEnv_t {
  // 协程调用链，这个数组最大128个元素，对一个线程来说，这个数组是唯一的，每次resume都会向里边压入一个协程指针，
  // yield的时候弹出。单线程执行的时候，在A协程内唤醒B协程，B又唤醒C......，这样的嵌套最多有128个，虽然很难遇
  // 到溢出情况，也还是需要注意一下
  stCoRoutine_t* pCallStack[128]; // 该线程内允许嵌套创建128个协程(即协程1内创建协程2, 协程2内创建协程3... 
                                  // 协程127内创建协程128. 该结构虽然是数组, 但将其作为栈来使用, 满足后进先出的特点)
  int iCallStackSize;  // 该线程内嵌套创建的协程数量, 即 pCallStack 数组中元素的数量

  // 协程调度器
  stCoEpoll_t* pEpoll; // 该线程内的epoll实例(套接字通过该结构内的epoll句柄向内核注册事件), 
                       // 也用于该线程的事件循环eventloop中，使用kqueue()实现

  // for copy stack log lastco and nextco
  stCoRoutine_t* pending_co; // 挂起的协程
  stCoRoutine_t* occupy_co;  // 当前协程(占据)
};

// int socket(int domain, int type, int protocol);

/**
 * co_log_err - 协程日志输出
 */
void co_log_err(const char* fmt, ...) {}

#if defined(__LIBCO_RDTSCP__)
static unsigned long long counter(void) {
  register uint32_t lo, hi;
  register unsigned long long o;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi)::"%rcx");
  o = hi;
  o <<= 32;
  return (o | lo);
}
static unsigned long long getCpuKhz() {
  FILE* fp = fopen("/proc/cpuinfo", "r");
  if (!fp)
    return 1;
  char buf[4096] = {0};
  fread(buf, 1, sizeof(buf), fp);
  fclose(fp);

  char* lp = strstr(buf, "cpu MHz");
  if (!lp)
    return 1;
  lp += strlen("cpu MHz");
  while (*lp == ' ' || *lp == '\t' || *lp == ':') {
    ++lp;
  }

  double mhz = atof(lp);
  unsigned long long u = (unsigned long long)(mhz * 1000);
  return u;
}
#endif

static unsigned long long GetTickMS() {
#if defined(__LIBCO_RDTSCP__)
  static uint32_t khz = getCpuKhz();
  return counter() / khz;
#else
  struct timeval now = {0};
  gettimeofday(&now, NULL);
  unsigned long long u = now.tv_sec;
  u *= 1000;
  u += now.tv_usec / 1000;
  return u;
#endif
}

/* no longer use
static pid_t GetPid() {
  static __thread pid_t pid = 0;
  static __thread pid_t tid = 0;
  if(!pid || !tid || pid != getpid()) {
    pid = getpid();
#if defined( __APPLE__ )
    tid = syscall( SYS_gettid );
    if (-1 == (long)tid) {
      tid = pid;
    }
#elif defined( __FreeBSD__ )
    syscall(SYS_thr_self, &tid);
    if(tid < 0) {
      tid = pid;
    }
#else
    tid = syscall( __NR_gettid );
#endif
  }
  return tid;
}

static pid_t GetPid() {
  char** p = (char**) pthread_self();
  return p ? *(pid_t*)(p + 18) : getpid();
}
*/

template<class T, class TLink>
void RemoveFromLink(T* ap) {
  TLink* lst = ap->pLink;
  if (!lst)
    return;
  assert(lst->head && lst->tail);

  if (ap == lst->head) {
    lst->head = ap->pNext;
    if (lst->head) {
      lst->head->pPrev = NULL;
    }
  } else {
    if (ap->pPrev) {
      ap->pPrev->pNext = ap->pNext;
    }
  }

  if (ap == lst->tail) {
    lst->tail = ap->pPrev;
    if (lst->tail) {
      lst->tail->pNext = NULL;
    }
  } else {
    ap->pNext->pPrev = ap->pPrev;
  }

  ap->pPrev = ap->pNext = NULL;
  ap->pLink = NULL;
}

template<typename TNode, typename TLink>
void inline AddTail(TLink* apLink, TNode* ap) {
  if (ap->pLink) {
    return;
  }
  if (apLink->tail) {
    apLink->tail->pNext = (TNode*) ap;
    ap->pNext = NULL;
    ap->pPrev = apLink->tail;
    apLink->tail = ap;
  } else {
    apLink->head = apLink->tail = ap;
    ap->pNext = ap->pPrev = NULL;
  }
  ap->pLink = apLink;
}

template<typename TNode, typename TLink> 
void inline PopHead(TLink* apLink) {
  if (!apLink->head) {
    return;
  }
  TNode* lp = apLink->head;
  if (apLink->head == apLink->tail) {
    apLink->head = apLink->tail = NULL;
  } else {
    apLink->head = apLink->head->pNext;
  }

  lp->pPrev = lp->pNext = NULL;
  lp->pLink = NULL;

  if (apLink->head) {
    apLink->head->pPrev = NULL;
  }
}

template<class TNode, class TLink>
void inline Join(TLink* apLink, TLink* apOther) {
  // printf("apOther %p\n",apOther);
  if (!apOther->head) {
    return;
  }
  TNode* lp = apOther->head;
  while (lp) {
    lp->pLink = apLink;
    lp = lp->pNext;
  }
  lp = apOther->head;
  if (apLink->tail) {
    apLink->tail->pNext = (TNode*) lp;
    lp->pPrev = apLink->tail;
    apLink->tail = apOther->tail;
  } else {
    apLink->head = apOther->head;
    apLink->tail = apOther->tail;
  }

  apOther->head = apOther->tail = NULL;
}

/////////////////for copy stack //////////////////////////
/**
 * 为协程创建一个栈
 * @param stack_size 栈大小
 */
stStackMem_t* co_alloc_stackmem(unsigned int stack_size) { // TODO: 这里继续......
  stStackMem_t* stack_mem = (stStackMem_t*) malloc(sizeof(stStackMem_t));
  stack_mem->occupy_co = NULL;
  stack_mem->stack_size = stack_size;
  stack_mem->stack_buffer = (char*) malloc(stack_size);
  stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
  return stack_mem;
}

/**
 * 初始化当前线程(中所有的协程)的共享栈(当前线程中的所有协程共享的栈)
 * @param count      预分配的 count 个协程栈
 * @param stack_size 每个协程栈大小
 */
stShareStack_t* co_alloc_sharestack(int count, int stack_size) {
  stShareStack_t* share_stack = (stShareStack_t*) malloc(sizeof(stShareStack_t));
  share_stack->alloc_idx = 0;
  share_stack->stack_size = stack_size;

  // alloc stack array
  share_stack->count = count;
  stStackMem_t** stack_array = (stStackMem_t**) calloc(count, sizeof(stStackMem_t*));
  for (int i = 0; i < count; i++) {
    stack_array[i] = co_alloc_stackmem(stack_size);
  }
  share_stack->stack_array = stack_array;
  return share_stack;
}

static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack) {
  if (!share_stack) {
    return NULL;
  }
  int idx = share_stack->alloc_idx % share_stack->count;
  share_stack->alloc_idx++;

  return share_stack->stack_array[idx];
}

// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;

/**
 * 线程epoll实例 - 该结构存在于 stCoRoutineEnv_t 结构中
 * 同一线程内所有的套接字都通过 iEpollFd 文件描述符 向内核注册事件
 */
struct stCoEpoll_t {
  int iEpollFd; // 由 epoll_create()函数创建的epoll句柄
  static const int _EPOLL_SIZE = 1024 * 10; // 10K

  struct stTimeout_t* pTimeout;
  struct stTimeoutItemLink_t* pstTimeoutList;
  struct stTimeoutItemLink_t* pstActiveList;
  
  co_epoll_res* result;
};

typedef void (*OnPreparePfn_t) (stTimeoutItem_t*, struct epoll_event& ev, stTimeoutItemLink_t* active);
typedef void (*OnProcessPfn_t) (stTimeoutItem_t*);

/**
 * 详见stPoll_t结构说明 
 */
struct stTimeoutItem_t {

  enum {
    eMaxTimeout = 40 * 1000 // 40s
  };

  stTimeoutItem_t* pPrev;
  stTimeoutItem_t* pNext;
  stTimeoutItemLink_t* pLink;

  unsigned long long ullExpireTime;

  OnPreparePfn_t pfnPrepare;
  OnProcessPfn_t pfnProcess;

  void* pArg; // routine，设置为当前线程中当前正在执行的协程
  bool bTimeout;
};

struct stTimeoutItemLink_t {
  stTimeoutItem_t* head;
  stTimeoutItem_t* tail;
};

struct stTimeout_t {
  stTimeoutItemLink_t* pItems;
  int iItemSize;

  unsigned long long ullStart;
  long long llStartIdx;
};
stTimeout_t* AllocTimeout(int iSize) {
  stTimeout_t* lp = (stTimeout_t*) calloc(1, sizeof(stTimeout_t));

  lp->iItemSize = iSize;
  lp->pItems = (stTimeoutItemLink_t*) calloc(1, sizeof(stTimeoutItemLink_t) * lp->iItemSize);

  lp->ullStart = GetTickMS();
  lp->llStartIdx = 0;

  return lp;
}

void FreeTimeout(stTimeout_t* apTimeout) {
  free(apTimeout->pItems);
  free(apTimeout);
}

int AddTimeout(stTimeout_t* apTimeout, stTimeoutItem_t* apItem, unsigned long long allNow) {
  if (apTimeout->ullStart == 0) {
    apTimeout->ullStart = allNow;
    apTimeout->llStartIdx = 0;
  }
  if (allNow < apTimeout->ullStart) {
    co_log_err(
        "CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
        __LINE__, allNow, apTimeout->ullStart);

    return __LINE__;
  }
  if (apItem->ullExpireTime < allNow) {
    co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow "
               "%llu apTimeout->ullStart %llu",
               __LINE__, apItem->ullExpireTime, allNow, apTimeout->ullStart);

    return __LINE__;
  }
  unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;

  if (diff >= (unsigned long long)apTimeout->iItemSize) {
    diff = apTimeout->iItemSize - 1;
    co_log_err("CO_ERR: AddTimeout line %d diff %d", __LINE__, diff);

    // return __LINE__;
  }
  AddTail(apTimeout->pItems + (apTimeout->llStartIdx + diff) % apTimeout->iItemSize, apItem);

  return 0;
}

inline void TakeAllTimeout(stTimeout_t* apTimeout, unsigned long long allNow, stTimeoutItemLink_t* apResult) {
  if (apTimeout->ullStart == 0) {
    apTimeout->ullStart = allNow;
    apTimeout->llStartIdx = 0;
  }

  if (allNow < apTimeout->ullStart) {
    return;
  }
  int cnt = allNow - apTimeout->ullStart + 1;
  if (cnt > apTimeout->iItemSize) {
    cnt = apTimeout->iItemSize;
  }
  if (cnt < 0) {
    return;
  }
  for (int i = 0; i < cnt; i++) {
    int idx = (apTimeout->llStartIdx + i) % apTimeout->iItemSize;
    Join<stTimeoutItem_t, stTimeoutItemLink_t>(apResult, apTimeout->pItems + idx);
  }
  apTimeout->ullStart = allNow;
  apTimeout->llStartIdx += cnt - 1;
}

/**
 * CoRoutineFunc - 所有新协程第一次被调度执行时的入口函数, 新协程在该入口函数中被执行
 * @param co -        (input) 第一次被调度的协程
 * @param 未命名指针 - (input) 用于兼容函数类型
 */
static int CoRoutineFunc(stCoRoutine_t* co, void*) {
  if (co->pfn) {
    co->pfn(co->arg);
  }
  co->cEnd = 1;

  stCoRoutineEnv_t* env = co->env; // 获取当前线程的调度器
  
  co_yield_env(env); // 删除调度器的协程数组中最后一个协程

  return 0;
}

/**
 * co_create_env - 分配协程存储空间(stCoRoutine_t)并初始化其中的部分成员变量
 * @param env - (input) 当前线程环境,用于初始化协程存储结构stCoRoutine_t
 * @param pfn - (input) 协程函数,用于初始化协程存储结构stCoRoutine_t
 * @param arg - (input) 协程函数的参数,用于初始化协程存储结构stCoRoutine_t
 * @return stCoRoutine_t类型的指针，返回一个协程对象实例
 */
struct stCoRoutine_t* co_create_env(stCoRoutineEnv_t* env,
                                    const stCoRoutineAttr_t* attr,
                                    pfn_co_routine_t pfn, void* arg) {

  stCoRoutineAttr_t at;
  if (attr) {
    memcpy(&at, attr, sizeof(at));
  }
  if (at.stack_size <= 0) { // 128k
    at.stack_size = 128 * 1024;
  } else if (at.stack_size > 1024 * 1024 * 8) { // 8M
    at.stack_size = 1024 * 1024 * 8;
  }

  if (at.stack_size & 0xFFF) {
    at.stack_size &= ~0xFFF;
    at.stack_size += 0x1000;
  }

  stCoRoutine_t* lp = (stCoRoutine_t*) malloc(sizeof(stCoRoutine_t));

  memset(lp, 0, (long) (sizeof(stCoRoutine_t)));

  lp->env = env;
  lp->pfn = pfn;
  lp->arg = arg;

  stStackMem_t* stack_mem = NULL;
  if (at.share_stack) { // 共享栈模式
    stack_mem = co_get_stackmem(at.share_stack);
    at.stack_size = at.share_stack->stack_size;
  } else { // 独享栈模式
    stack_mem = co_alloc_stackmem(at.stack_size);
  }
  lp->stack_mem = stack_mem; // 独享栈

  lp->ctx.ss_sp = stack_mem->stack_buffer;
  lp->ctx.ss_size = at.stack_size;

  lp->cStart = 0;
  lp->cEnd = 0;
  lp->cIsMain = 0;
  lp->cEnableSysHook = 0;
  lp->cIsShareStack = at.share_stack != NULL;

  lp->save_size = 0;
  lp->save_buffer = NULL;

  return lp;
}

/**
 * 创建协程，接口样式模拟的是pthread
 */
int co_create(stCoRoutine_t** ppco, const stCoRoutineAttr_t* attr, pfn_co_routine_t pfn, void* arg) {
  if (!co_get_curr_thread_env()) {
    co_init_curr_thread_env();
  }
  stCoRoutine_t* co = co_create_env(co_get_curr_thread_env(), attr, pfn, arg);
  *ppco = co;
  return 0;
}

void co_free(stCoRoutine_t* co) {
  if (!co->cIsShareStack) {
    free(co->stack_mem->stack_buffer);
    free(co->stack_mem);
  } else { // walkerdu fix at 2018-01-20 存在内存泄漏
    if (co->save_buffer) {
      free(co->save_buffer);
    }

    if (co->stack_mem->occupy_co == co) {
      co->stack_mem->occupy_co = NULL;
    }
  }

  free(co);
}
void co_release(stCoRoutine_t* co) { 
  co_free(co); 
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);

void co_resume(stCoRoutine_t* co) {
  stCoRoutineEnv_t* env = co->env;
  stCoRoutine_t* lpCurrRoutine = env->pCallStack[env->iCallStackSize - 1];
  if (!co->cStart) {
    coctx_make(&co->ctx, (coctx_pfn_t) CoRoutineFunc, co, 0);
    co->cStart = 1;
  }
  env->pCallStack[env->iCallStackSize++] = co;
  co_swap(lpCurrRoutine, co); // 切换协程
}

// walkerdu 2018-01-14
// 用于reset超时无法重复使用的协程
void co_reset(stCoRoutine_t* co) {
  if (!co->cStart || co->cIsMain)
    return;

  co->cStart = 0;
  co->cEnd = 0;

  // 如果当前协程有共享栈被切出的buff，要进行释放
  if (co->save_buffer) {
    free(co->save_buffer);
    co->save_buffer = NULL;
    co->save_size = 0;
  }

  // 如果共享栈被当前协程占用，要释放占用标志，否则被切换，会执行save_stack_buffer()
  if (co->stack_mem->occupy_co == co)
    co->stack_mem->occupy_co = NULL;
}

/**
 * 让出当前协程，执行上一条协程
 * @param env 当前线程上下文，例如：正在运行的协程队列
 */
void co_yield_env(stCoRoutineEnv_t* env) {
  stCoRoutine_t* last = env->pCallStack[env->iCallStackSize - 2];
  stCoRoutine_t* curr = env->pCallStack[env->iCallStackSize - 1];

  env->iCallStackSize--;

  co_swap(curr, last);
}

void co_yield_ct() { co_yield_env(co_get_curr_thread_env()); }
void co_yield(stCoRoutine_t* co) {
  co_yield_env(co->env); 
}

void save_stack_buffer(stCoRoutine_t* occupy_co) {
  /// copy out
  stStackMem_t* stack_mem = occupy_co->stack_mem;
  int len = stack_mem->stack_bp - occupy_co->stack_sp;

  if (occupy_co->save_buffer) {
    free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
  }

  occupy_co->save_buffer = (char*) malloc(len); // malloc buf;
  occupy_co->save_size = len;

  memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

/**
 * - 切换协程上下文
 * 协程的切换都是通过内部调用co_swap()函数来完成，具体的切换过程分两种情况：（假设协程 co_from 切换到 co_to）
 * 
 * - 如果是独享栈模式：
 * 1、将协程 co_from 当前的 CPU 寄存器信息全部存入 co_from 中(具体字段是：stCoRoutine_t.ctx字段)。
 * 2、将协程 co_to 的寄存器信息赋值到 CPU 寄存器内。
 * 3、当执行下一行代码的时候，已经切换到了 co_to 协程，栈帧也指向了 co_to 的栈帧空间了。
 * 注意：独享栈模式特点是：性能好(无需copy栈)；但有oom风险(每个协程一个独立的128K的栈在heap上分配)
 * 
 * - 如果是共享栈模式：可以在一台机器上支持更多的并发量(因为不怕oom)
 * libco对共享栈做了个优化，可以申请多个共享栈循环使用，当目标协程所记录的共享栈没有被其它协程占用的时候，整个切换
 * 过程和独享栈模式一致。否则就是下面的流程:
 * 1、将协程 co_from 的栈空间内容从共享栈拷贝到 co_from 的 save_buffer 中。
 * 2、将协程 co_to 的 save_buffer 中的栈内容拷贝到共享栈中。
 * 3、将协程 co_from 当前的 CPU 寄存器信息全部存入 co_from 的 ctx 中。
 * 4、将协程 co_to 的寄存器信息赋值到 CPU 寄存器内。
 * 5、当执行下一行代码的时候，已经切换到了 co_to 协程，栈帧一直指向共享栈间。
 * 
 * - 其中寄存器的拷贝切换都是通过 coctx_swap 汇编实现
 * 
 * - libco两种栈模式的优缺点：
 * 独享栈：
 * 优点：协程切换的时候，只需要切换寄存器数据，效率高。
 * 缺点：1、每个协程独占128K的固定空间，协程池足够大的时候，内存浪费比较大； 
 *      2、容易发生堆栈溢出。
 * 共享栈：
 * 优点：可以创建多个足够大的共享栈空间，很大程度上缓解了内存占用问题，也减小了栈溢出的几率。
 * 缺点：协程切换的时候，可能会多两次栈内容拷贝，拉低了性能
 * 
 * - 案例：游戏项目
 * libco用在项目中可能会遇到的问题和解决方案：
 * 一般在游戏项目中，服务器的效率是优先考虑的，所以一般会选择 stackfull 独享栈模式，所以下面的问题都是基于独享栈模式的。
 * 1、可能的栈溢出：游戏里常会用临时变量或者容器存储数据，如果协程调用函数足够深的话，累积栈容量有可能会超过128K，导致程序异常。
 * 解决方案a：最简单粗暴的扩大栈空间比如512K，这种方法用更大的内存占用来降低爆栈风险。得不偿失，而且只是降低爆栈风险而已，并不
 *          能完全解决问题，不推荐。
 * 解决方案b：把常用变量类型和自定义类型都包装一下，改变内存分配方式为在堆内分配，参考：改造局部变量的内存分配方式，这样基本可以
 *          解决栈溢出的问题，但是协程化的时候，代码变动比较多，而且对开发者的约束也比较多，不太方便。
 * 解决方案c：找一个合适的时机，计算一下当前栈使用情况，如果占用空间超过阈值比如80%的时候，申请一块更大的内存代替当前栈空间，并
 *          把相关寄存器和栈内容拷贝过来，销毁旧的栈空间。等协程回收的时候，再将扩大的栈缩小为默认大小。参考 libco栈自动扩容。
 *          这种解决方案有个缺陷，就是不能有 指向栈变量的指针或引用，因为栈拷贝之后，指针指向的数据位置已经变化了，但是指针的
 *          值没有变化，是有很大安全隐患的。
 * 解决方案d：利用 malloc 的一个特性，就是 malloc 内存的时候，写数据之前，其实只是从虚拟地址空间里分配了一段地址范围，没有实
 *          际分配物理内存，只有实际写入数据的时候才发生缺页中断，分配需要大小的物理内存。利用这个特性，可以让协程先分配一个比
 *          较大的空间，比如8M。当大量的协程创建的时候，总的分配出来内存可以远大于实际物理内存，64位服务器上最多可以分配 
 *          2^48 bit的虚拟地址空间，所以是够用的。实际运行过程中大部分协程实际分配的物理内存都是在一定阈值之内，比如128K，偶
 *          尔有些超出阈值的，只要没有超过8M，就不会有堆栈溢出。对于这些超出阈值分配的空间，每次协程回收的时候都检测一下是否有
 *          超出，如果超出的话，做一个回收就可以保证物理内存在一个可控范围之内。这里会用到两个系统调用：mincore()和madvise()。
 *          因为超出阈值分配的协程肯定是小概率的，所以对服务器性能的影响很小。具体实现方案参考 让协程私有栈足够大。推荐这种实现方案。
 * 以上的所有解决方案都是最大程度防止溢出，并不能完全避免。当有代码突然分配一个超大的栈空间的时候，还是会出现溢出。而且当程序发生栈溢
 * 出的时候，如果溢出部分写入的内存是可写的，往往不会直接触发bug，只有当被写坏的内存数据被使用的时候才会发现bug，但这个时候已经不在事
 * 故现场了。为了能及时发现这种bug，在分配的栈空间的栈顶，对libco来说，就是在分配出来的栈空间的低地址位置，设置一定空间比如 4K 为只
 * 读。这样当将要发生溢出，第一次写这块内存的时候就会触发异常抛出，及时发现bug现场。具体实现方案需要一个系统调用 mprotect()，只需要
 * 第一次创建协程的时候调用一次，不会影响整个运行效率。
 * 2、数据重入： 协程中如果需要一个全局的数据做计算的话，在协程挂起，再唤醒的时候，这个数据可能已经被其它协程改写，影响当前协程的计算。
 * 如果是普通数据类型，可以用 libco 的协程级私有变量 ROUTINE_VAR 快速改造，避免重入；但是在游戏中经常有一种场景：假设需要遍历所有
 * 玩家，对每个玩家做个异步的操作，比如落地一个置脏数据，协程化之后，就变成用迭代器迭代一个全局容器，在迭代循环内有协程挂起操作。这样在
 * 协程唤醒继续循环的时候，可能容器本身已经发生了变化，使当前的迭代器指向一个错误的成员导致bug发生。
 * 解决方案：尽量不在遍历全局容器的过程中触发协程切换；如果不是逻辑明确需要，尽量不在协程内声明指向全局数据的指针或引用。
 * 
 * - libco的使用
 * libco的接口封装很简单，一般使用只涉及4个接口：
 * co_create 创建一个协程；co_yield 挂起一个协程；co_resume 唤醒一个协程；co_release 销毁一个协程。
 * 具体用法可以参考官方用例。
 * 
 * 函数切换时需要关心的仅仅是：函数的栈帧 + 寄存器
 * 
 * @param curr 当前协程，co_from
 * @param pending_co 下一个协程 co_to
 */
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co) {
  stCoRoutineEnv_t* env = co_get_curr_thread_env(); // 当前thread中的所有协程数据(状态、信息)

  // get curr stack sp
  char c;
  curr->stack_sp = &c;

  if (!pending_co->cIsShareStack) { // 独享栈模式
    env->pending_co = NULL;
    env->occupy_co = NULL;
  } else { // 共享栈模式
    env->pending_co = pending_co;
    // get last occupy co on the same stack mem
    stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
    // set pending co to occupy thest stack mem;
    pending_co->stack_mem->occupy_co = pending_co;

    env->occupy_co = occupy_co;
    if (occupy_co && occupy_co != pending_co) {
      save_stack_buffer(occupy_co);
    }
  }

  // FIXME: swap context 切换协程上下文
  coctx_swap(&(curr->ctx), &(pending_co->ctx));

  // stack buffer may be overwrite, so get again;
  stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
  stCoRoutine_t* update_occupy_co = curr_env->occupy_co;
  stCoRoutine_t* update_pending_co = curr_env->pending_co;

  if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co) {
    // resume stack buffer
    if (update_pending_co->save_buffer && update_pending_co->save_size > 0) {
      memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
    }
  }
}

// int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t;
struct stPoll_t : public stTimeoutItem_t {
  struct pollfd* fds;
  nfds_t nfds; // typedef unsigned long int nfds_t;

  stPollItem_t* pPollItems;

  int iAllEventDetach;

  int iEpollFd;

  int iRaiseCnt;
};

struct stPollItem_t : public stTimeoutItem_t {
  struct pollfd* pSelf;
  stPoll_t* pPoll;

  struct epoll_event stEvent;
};

/**
 * EPOLLPRI 		POLLPRI    // There is urgent data to read.
 * EPOLLMSG 		POLLMSG
 *
 * POLLREMOVE
 * POLLRDHUP
 * POLLNVAL
 */
static uint32_t PollEvent2Epoll(short events) {
  uint32_t e = 0;
  if (events & POLLIN)
    e |= EPOLLIN;
  if (events & POLLOUT)
    e |= EPOLLOUT;
  if (events & POLLHUP)
    e |= EPOLLHUP;
  if (events & POLLERR)
    e |= EPOLLERR;
  if (events & POLLRDNORM)
    e |= EPOLLRDNORM;
  if (events & POLLWRNORM)
    e |= EPOLLWRNORM;
  return e;
}

static short EpollEvent2Poll(uint32_t events) {
  short e = 0;
  if (events & EPOLLIN)
    e |= POLLIN;
  if (events & EPOLLOUT)
    e |= POLLOUT;
  if (events & EPOLLHUP)
    e |= POLLHUP;
  if (events & EPOLLERR)
    e |= POLLERR;
  if (events & EPOLLRDNORM)
    e |= POLLRDNORM;
  if (events & EPOLLWRNORM)
    e |= POLLWRNORM;
  return e;
}

// __thread 表示线程私有变量
// libco 的协程是 "单调用链"的，就是说一个线程内可以创建 N 个协程，协程总是由当前线程调用，一个线程只有一条调用链。
// libco 使用 stCoRoutineEnv_t 结构来记录当前的调用链。当前线程的调用链通过线程专有变量 gCoEnvPerThread 来保存，
// libco 内部会在第一次使用协程的时候初始化这个变量，我们看到 stCoRoutineEnv_t.pCallStack 的大小 128，这意味着，
// 我们最多可以在协程嵌套 co_resume 新协程的深度为 128（协程里面运行新的协程）
static __thread stCoRoutineEnv_t* gCoEnvPerThread = nullptr; // 表示每条线程中的协程上下文

/**
 * 初始化当前线程的协程环境
 */
void co_init_curr_thread_env() {
  gCoEnvPerThread = (stCoRoutineEnv_t*) calloc(1, sizeof(stCoRoutineEnv_t));
  stCoRoutineEnv_t* env = gCoEnvPerThread;

  env->iCallStackSize = 0;
  struct stCoRoutine_t* self = co_create_env(env, NULL, NULL, NULL);
  self->cIsMain = 1;

  env->pending_co = NULL;
  env->occupy_co = NULL;

  coctx_init(&self->ctx);

  env->pCallStack[env->iCallStackSize++] = self;

  stCoEpoll_t* ev = AllocEpoll();
  SetEpoll(env, ev);
}

/**
 * 获取当前线程上下文，其中包括：该线程中的所有协程调用栈(栈帧)、epoll等
 */
stCoRoutineEnv_t* co_get_curr_thread_env() { 
  return gCoEnvPerThread;
}

/**
 * Poll处理事件
 */
void OnPollProcessEvent(stTimeoutItem_t* ap) {
  stCoRoutine_t* co = (stCoRoutine_t*) ap->pArg; // 获取当前正在运行的协程对象实例
  co_resume(co); // resume当前这个协程实例
}

void OnPollPreparePfn(stTimeoutItem_t* ap, struct epoll_event& e, stTimeoutItemLink_t* active) {
  stPollItem_t* lp = (stPollItem_t*) ap;
  lp->pSelf->revents = EpollEvent2Poll(e.events);

  stPoll_t* pPoll = lp->pPoll;
  pPoll->iRaiseCnt++;

  if (!pPoll->iAllEventDetach) {
    pPoll->iAllEventDetach = 1;

    RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(pPoll);

    AddTail(active, pPoll);
  }
}

/**
 * 开始 "事件循环"
 */
void co_eventloop(stCoEpoll_t* ctx, pfn_co_eventloop_t pfn, void* arg) {
  if (!ctx->result) {
    ctx->result = co_epoll_res_alloc(stCoEpoll_t::_EPOLL_SIZE);
  }
  co_epoll_res* result = ctx->result;

  for (;;) {
    int ret = co_epoll_wait(ctx->iEpollFd, result, stCoEpoll_t::_EPOLL_SIZE, 1);

    stTimeoutItemLink_t* active = (ctx->pstActiveList);
    stTimeoutItemLink_t* timeout = (ctx->pstTimeoutList);

    memset(timeout, 0, sizeof(stTimeoutItemLink_t));

    for (int i = 0; i < ret; i++) {
      stTimeoutItem_t* item = (stTimeoutItem_t*) result->events[i].data.ptr;
      if (item->pfnPrepare) {
        item->pfnPrepare(item, result->events[i], active);
      } else {
        AddTail(active, item);
      }
    }

    unsigned long long now = GetTickMS();
    TakeAllTimeout(ctx->pTimeout, now, timeout);

    stTimeoutItem_t* lp = timeout->head;
    while (lp) {
      // printf("raise timeout %p\n",lp);
      lp->bTimeout = true;
      lp = lp->pNext;
    }

    Join<stTimeoutItem_t, stTimeoutItemLink_t>(active, timeout);

    lp = active->head;
    while (lp) {
      PopHead<stTimeoutItem_t, stTimeoutItemLink_t>(active);
      if (lp->bTimeout && now < lp->ullExpireTime) {
        int ret = AddTimeout(ctx->pTimeout, lp, now);
        if (!ret) {
          lp->bTimeout = false;
          lp = active->head;
          continue;
        }
      }
      if (lp->pfnProcess) {
        lp->pfnProcess(lp);
      }

      lp = active->head;
    }
    if (pfn) {
      if (-1 == pfn(arg)) {
        break;
      }
    }
  }
}

void OnCoroutineEvent(stTimeoutItem_t* ap) {
  stCoRoutine_t* co = (stCoRoutine_t*) ap->pArg;
  co_resume(co);
}

stCoEpoll_t* AllocEpoll() {
  stCoEpoll_t* ctx = (stCoEpoll_t*) calloc(1, sizeof(stCoEpoll_t));

  ctx->iEpollFd = co_epoll_create(stCoEpoll_t::_EPOLL_SIZE); // 通过kqueue()初始化
  ctx->pTimeout = AllocTimeout(60 * 1000);

  ctx->pstActiveList = (stTimeoutItemLink_t*) calloc(1, sizeof(stTimeoutItemLink_t));
  ctx->pstTimeoutList = (stTimeoutItemLink_t*) calloc(1, sizeof(stTimeoutItemLink_t));

  return ctx;
}

void FreeEpoll(stCoEpoll_t* ctx) {
  if (ctx) {
    free(ctx->pstActiveList);
    free(ctx->pstTimeoutList);
    FreeTimeout(ctx->pTimeout);
    co_epoll_res_free(ctx->result);
  }
  free(ctx);
}

/**
 * 获取当前协程(正在运行着的co-routine)的上下文环境
 */ 
stCoRoutine_t* GetCurrCo(stCoRoutineEnv_t* env) {
  return env->pCallStack[env->iCallStackSize - 1];
}

/**
 * 获取当前thread中所有的co-routin
 */
stCoRoutine_t* GetCurrThreadCo() {
  stCoRoutineEnv_t* env = co_get_curr_thread_env();
  if (!env) {
    return 0;
  }
  return GetCurrCo(env);
}

typedef int (*poll_pfn_t) (struct pollfd fds[], nfds_t nfds, int timeout);

/**
 * @param pollfunc 系统调用poll()
 */
int co_poll_inner(stCoEpoll_t* ctx, 
                  struct pollfd fds[], nfds_t nfds, 
                  int timeout, poll_pfn_t pollfunc) {
                    
  if (timeout == 0) {
    return pollfunc(fds, nfds, timeout);
  }
  if (timeout < 0) {
    timeout = INT_MAX;
  }
  int epfd = ctx->iEpollFd; // TODO: epoll的句柄(实际是kqueue)
  stCoRoutine_t* self = co_self(); // 获取当前需要执行的协程实例

  // 1.struct change
  stPoll_t& arg = *((stPoll_t*) malloc(sizeof(stPoll_t)));
  memset(&arg, 0, sizeof(arg));

  arg.iEpollFd = epfd;
  arg.fds = (pollfd*) calloc(nfds, sizeof(pollfd));
  arg.nfds = nfds;

  stPollItem_t arr[2];
  if (nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack) {
    // 意思是：只有一个fd需要poll 且 当前协程是独享栈模式
    arg.pPollItems = arr; // nfds < 2 && 没有使用共享栈，
  } else {
    arg.pPollItems = (stPollItem_t*) malloc(nfds * sizeof(stPollItem_t));
  }
  memset(arg.pPollItems, 0, nfds * sizeof(stPollItem_t));

  arg.pfnProcess = OnPollProcessEvent; // co_resume
  arg.pArg = GetCurrCo(co_get_curr_thread_env()); // stCoRoutine_t 当前协程实例

  // 2. add epoll
  for (nfds_t i = 0; i < nfds; i++) {
    arg.pPollItems[i].pSelf = arg.fds + i;
    arg.pPollItems[i].pPoll = &arg;

    arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
    struct epoll_event& ev = arg.pPollItems[i].stEvent;

    if (fds[i].fd > -1) {
      ev.data.ptr = arg.pPollItems + i;
      ev.events = PollEvent2Epoll(fds[i].events);

      int ret = co_epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i].fd, &ev);
      if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL) {
        if (arg.pPollItems != arr) {
          free(arg.pPollItems);
          arg.pPollItems = NULL;
        }
        free(arg.fds);
        free(&arg);
        return pollfunc(fds, nfds, timeout);
      }
    }
    // if fail,the timeout would work
  }

  // 3.add timeout

  unsigned long long now = GetTickMS();
  arg.ullExpireTime = now + timeout;
  int ret = AddTimeout(ctx->pTimeout, &arg, now);
  int iRaiseCnt = 0;
  if (ret != 0) {
    co_log_err(
        "CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
        ret, now, timeout, arg.ullExpireTime);

    errno = EINVAL;
    iRaiseCnt = -1;
  } else {
    co_yield_env(co_get_curr_thread_env());
    iRaiseCnt = arg.iRaiseCnt;
  }

  {
    // clear epoll status and memory
    RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(&arg);
    for (nfds_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd > -1) {
        co_epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &arg.pPollItems[i].stEvent);
      }
      fds[i].revents = arg.fds[i].revents;
    }

    if (arg.pPollItems != arr) {
      free(arg.pPollItems);
      arg.pPollItems = NULL;
    }

    free(arg.fds);
    free(&arg);
  }

  return iRaiseCnt;
}

int co_poll(stCoEpoll_t* ctx, struct pollfd fds[], nfds_t nfds, int timeout_ms) {
  return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll(stCoRoutineEnv_t* env, stCoEpoll_t* ev) {
  env->pEpoll = ev;
}

stCoEpoll_t* co_get_epoll_ct() {
  if (!co_get_curr_thread_env()) {
    co_init_curr_thread_env();
  }
  return co_get_curr_thread_env()->pEpoll;
}

struct stHookPThreadSpec_t {
  stCoRoutine_t* co;
  void* value;

  enum {
    size = 1024
  };
};

/**
 * 获取当前协程的私有变量
 */
void* co_getspecific(pthread_key_t key) {
  stCoRoutine_t* co = GetCurrThreadCo();
  if (!co || co->cIsMain) {
    return pthread_getspecific(key);
  }
  return co->aSpec[key].value;
}

/**
 * 存储协程私有变量
 */
int co_setspecific(pthread_key_t key, const void* value) {
  stCoRoutine_t* co = GetCurrThreadCo();
  if (!co || co->cIsMain) {
    return pthread_setspecific(key, value);
  }
  co->aSpec[key].value = (void*) value; // 存储在当前协程的私有变量中
  return 0;
}

void co_disable_hook_sys() {
  stCoRoutine_t* co = GetCurrThreadCo();
  if (co) {
    co->cEnableSysHook = 0;
  }
}

/**
 * 当前线程是否开启了Linux系统调用的hook机制？
 */
bool co_is_enable_sys_hook() {
  stCoRoutine_t* co = GetCurrThreadCo();
  return (co && co->cEnableSysHook);
}

/**
 * 获取当前线程中的当前正在执行的协程
 */
stCoRoutine_t* co_self() { 
  return GetCurrThreadCo(); 
}

// co cond
struct stCoCond_t;

struct stCoCondItem_t {
  stCoCondItem_t* pPrev;
  stCoCondItem_t* pNext;

  stCoCond_t* pLink; // TODO:
  stTimeoutItem_t timeout; // TODO:
};

struct stCoCond_t {
  stCoCondItem_t* head;
  stCoCondItem_t* tail;
};

static void OnSignalProcessEvent(stTimeoutItem_t* ap) {
  stCoRoutine_t* co = (stCoRoutine_t*) ap->pArg;
  co_resume(co);
}

stCoCondItem_t* co_cond_pop(stCoCond_t* link);
int co_cond_signal(stCoCond_t* si) {
  stCoCondItem_t* sp = co_cond_pop(si);
  if (!sp) {
    return 0;
  }
  RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(&sp->timeout);

  AddTail(co_get_curr_thread_env()->pEpoll->pstActiveList, &sp->timeout);

  return 0;
}

int co_cond_broadcast(stCoCond_t* si) {
  for (;;) {
    stCoCondItem_t* sp = co_cond_pop(si);
    if (!sp)
      return 0;

    RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(&sp->timeout);

    AddTail(co_get_curr_thread_env()->pEpoll->pstActiveList, &sp->timeout);
  }

  return 0;
}

int co_cond_timedwait(stCoCond_t* link, int ms) {
  stCoCondItem_t* psi = (stCoCondItem_t*) calloc(1, sizeof(stCoCondItem_t));
  psi->timeout.pArg = GetCurrThreadCo();
  psi->timeout.pfnProcess = OnSignalProcessEvent;

  if (ms > 0) {
    unsigned long long now = GetTickMS();
    psi->timeout.ullExpireTime = now + ms;

    int ret = AddTimeout(co_get_curr_thread_env()->pEpoll->pTimeout, &psi->timeout, now);
    if (ret != 0) {
      free(psi);
      return ret;
    }
  }
  AddTail(link, psi);

  co_yield_ct();

  RemoveFromLink<stCoCondItem_t, stCoCond_t>(psi);
  free(psi);

  return 0;
}

stCoCond_t* co_cond_alloc() {
  return (stCoCond_t*) calloc(1, sizeof(stCoCond_t));
}

int co_cond_free(stCoCond_t* cc) {
  free(cc);
  return 0;
}

stCoCondItem_t* co_cond_pop(stCoCond_t* link) {
  stCoCondItem_t* p = link->head;
  if (p) {
    PopHead<stCoCondItem_t, stCoCond_t>(link);
  }
  return p;
}
