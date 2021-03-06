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
#include <queue>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;

struct stTask_t {
  int id;
};

struct stEnv_t {
  stCoCond_t* cond; // 条件变量
  queue<stTask_t*> task_queue; // 任务队列
};

/**
 * 生产者
 * @param args 传入的是 stEnv_t
 */
void* Producer(void* args) {
  co_enable_hook_sys();

  stEnv_t* env = (stEnv_t*) args;
  int id = 0;
  while (true) {
    stTask_t* task = (stTask_t*) calloc(1, sizeof(stTask_t));
    task->id = id++;
    env->task_queue.push(task);
    printf("%s:%d produce task %d\n", __func__, __LINE__, task->id);
    co_cond_signal(env->cond);
    poll(NULL, 0, 1000);
  }
  return NULL;
}

/**
 * @param args 传入的是 stEnv_t
 */
void* Consumer(void* args) {
  co_enable_hook_sys();
  
  stEnv_t* env = (stEnv_t*) args;
  while (true) {
    if (env->task_queue.empty()) {
      co_cond_timedwait(env->cond, -1);
      continue;
    }
    stTask_t* task = env->task_queue.front();
    env->task_queue.pop();
    printf("%s:%d consume task %d\n", __func__, __LINE__, task->id);
    free(task);
  }
  return NULL;
}

int main() {
  stEnv_t* env = new stEnv_t;
  env->cond = co_cond_alloc();

  // 消费者协程
  stCoRoutine_t* consumer_routine;
  co_create(&consumer_routine, NULL, Consumer, env);
  co_resume(consumer_routine);

  // 生产者协程
  stCoRoutine_t* producer_routine;
  co_create(&producer_routine, NULL, Producer, env);
  co_resume(producer_routine);

  // 开启协程事件(处理)循环，协程创建启动完之后，我们需要执行epoll的事件循环处理，协助协程的调度及异步操作
  co_eventloop(co_get_epoll_ct(), NULL, NULL);
  return 0;
}
