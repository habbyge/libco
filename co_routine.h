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

#ifndef __CO_ROUTINE_H__
#define __CO_ROUTINE_H__

#include <pthread.h>
#include <stdint.h>
#include <sys/poll.h>

/**
 * libco模块划分(按协程三要素划分)：
 * 1、调度器：co_epoll
 * 2、上下文切换：coctx
 * 3、协程API：co_routine
 */

// 1.struct

struct stCoRoutine_t;
struct stShareStack_t;

struct stCoRoutineAttr_t {
  int stack_size;
  stShareStack_t* share_stack;

  stCoRoutineAttr_t() {
    stack_size = 128 * 1024; // 独享栈模式，每个创建的协程都会在堆上分配一块默认128K的内存作为自己的栈帧空间
    share_stack = NULL;
  }
} __attribute__((packed));
// _attribute__ ((packed)) 的作用就是告诉编译器取消结构在编译过程中的优化对齐，按照实际占用字节数进行对齐，
// 是GCC特有的语法。类似于在目标结构体前写 #pragma pack(1)，即按1字节对其。

struct stCoEpoll_t;
typedef int (*pfn_co_eventloop_t) (void*);
typedef void* (*pfn_co_routine_t) (void*);

// 2.co_routine
// 协程生命周期开始：co_create()指定协程入口函数并创建协程，co_resume 将其唤醒，开始执行当前协程。
int co_create(stCoRoutine_t** co, const stCoRoutineAttr_t* attr, void* (*routine) (void*), void* arg);

// 唤醒协程，开始执行，协程的切换都是通过内部调用 co_swap() 函数来完成
void co_resume(stCoRoutine_t* co);
void co_yield(stCoRoutine_t* co);
void co_yield_ct(); // ct = current thread

// 协程生命周期结束：主动调用co_release()，或者co_create()指定的入口函数执行完毕返回，协程结束。
void co_release(stCoRoutine_t* co);
void co_reset(stCoRoutine_t* co);

stCoRoutine_t* co_self();

int co_poll(stCoEpoll_t* ctx, struct pollfd fds[], nfds_t nfds, int timeout_ms);
void co_eventloop(stCoEpoll_t* ctx, pfn_co_eventloop_t pfn, void* arg);

// 3.specific

int co_setspecific(pthread_key_t key, const void* value);
void* co_getspecific(pthread_key_t key);

// 4.event

stCoEpoll_t* co_get_epoll_ct(); // ct = current thread

// 5.hook syscall ( poll/read/write/recv/send/recvfrom/sendto )

void co_enable_hook_sys();
void co_disable_hook_sys();
bool co_is_enable_sys_hook();

// 6.sync
struct stCoCond_t;

stCoCond_t* co_cond_alloc();
int co_cond_free(stCoCond_t* cc);

int co_cond_signal(stCoCond_t*);
int co_cond_broadcast(stCoCond_t*);
int co_cond_timedwait(stCoCond_t*, int timeout_ms);

// 7.share stack
stShareStack_t* co_alloc_sharestack(int iCount, int iStackSize);

// 8.init envlist for hook get/set env
void co_set_env_list(const char* name[], size_t cnt);

void co_log_err(const char *fmt, ...);
#endif
