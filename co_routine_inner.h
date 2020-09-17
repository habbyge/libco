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

#ifndef __CO_ROUTINE_INNER_H__

#include "co_routine.h"
#include "coctx.h"
struct stCoRoutineEnv_t;
struct stCoSpec_t {
  void* value;
};

/**
 * 栈空间
 */
struct stStackMem_t {
  stCoRoutine_t* occupy_co; // 独占该栈空间的协程
  int stack_size;

  char* stack_bp; // stack_buffer + stack_size
  char* stack_buffer;
};

/**
 * 共享栈(stackless)模式
 * stackless：共享栈模式，所有协程共享 count 块提前分配好的大内存作为栈帧空间(stackarray)
 */
struct stShareStack_t {
  unsigned int alloc_idx;
  int stack_size;
  int count;
  stStackMem_t** stack_array;
};

/**
 * 协程实例
 * 每个协程本质上就是一个结构体stCoRoutine_t。
 * 该结构表示某个协程实例的具体内容，创建协程的时候会生成一个该结构体，所有协程的生命周期都是围绕这个结构体来的。
 */
struct stCoRoutine_t {
  stCoRoutineEnv_t* env; // 协程环境(协程调度器？)
  pfn_co_routine_t pfn; // 表示该协程对应的执行函数指针
  void* arg;

  // 保存当前协程执行时的所有寄存器
  coctx_t ctx; // 存储的是当前协程的上下文，在调用co_swap()时使用

  char cStart;
  char cEnd;
  
  char cIsMain; // 主协程？
  
  char cEnableSysHook; // 是能系统api hook机制

  // libco有两种栈管理方案：stackless(共享栈模式) and stackfull(独享站模式)
  char cIsShareStack; // 是否使用协程的共享栈模式(stackless)

  void* pvEnv;

  // char sRunStack[ 1024 * 128 ];
  // 如果是独享栈模式，分配在堆中的一块作为当前协程栈帧的内存 stack_mem，这块内存的默认大小为 128K。
  stStackMem_t* stack_mem;

  // save satck buffer while confilct on same stack_buffer;
  char* stack_sp;

  // 如果是共享栈模式，协程切换的时候，用来拷贝存储当前共享栈内容的 save_buffer，长度为实际的共享栈使用长度。
  char* save_buffer;
  unsigned int save_size;

  stCoSpec_t aSpec[1024]; // 协程私有变量
};

// 1.env
void co_init_curr_thread_env();
stCoRoutineEnv_t* co_get_curr_thread_env();

// 2.coroutine
void co_free(stCoRoutine_t* co);
void co_yield_env(stCoRoutineEnv_t* env);

// 3.func

//-----------------------------------------------------------------------------------------------

struct stTimeout_t;
struct stTimeoutItem_t;

stTimeout_t* AllocTimeout(int iSize);
void FreeTimeout(stTimeout_t* apTimeout);
int AddTimeout(stTimeout_t* apTimeout, stTimeoutItem_t* apItem, uint64_t allNow);

struct stCoEpoll_t;
stCoEpoll_t* AllocEpoll();
void FreeEpoll(stCoEpoll_t* ctx);

stCoRoutine_t* GetCurrThreadCo();
void SetEpoll(stCoRoutineEnv_t* env, stCoEpoll_t* ev);

typedef void (*pfnCoRoutineFunc_t)();

#endif

#define __CO_ROUTINE_INNER_H__
