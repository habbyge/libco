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

#pragma once
#include <pthread.h>
#include <stdlib.h>

/*
invoke only once in the whole program
CoRoutineSetSpecificCallBack(CoRoutineGetSpecificFunc_t
pfnGet,CoRoutineSetSpecificFunc_t pfnSet)

struct MyData_t {
	int iValue;
  char szValue[100];
};
CO_ROUTINE_SPECIFIC( MyData_t,__routine );

int main() {
  CoRoutineSetSpecificCallBack( co_getspecific,co_setspecific );

  __routine->iValue = 10;
  strcpy( __routine->szValue,"hello world" );

  return 0;
}
*/

// TODO: 指定name类型的协程特私有变量y，有啥用？提供了一个宏让用户可以方便地使用协程私有变量。可以看到，
// pthread_key_t 是需要用到 "线程私有变量" 的相关设施来创建的。可以看到相关实现非常简单，如果是主协程
// 则直接使用线程私有变量的相关函数，否则使用协程结构（上面有给出协程结构的定义）的 aSpec 数组来保存，
// aSpec 是一个大小为 1024 的数组，其数组元素类型为 stCoSpec_t。

extern int co_setspecific(pthread_key_t key, const void* value);
extern void* co_getspecific(pthread_key_t key);

#define CO_ROUTINE_SPECIFIC(name, y)                                           \
                                                                               \
  static pthread_once_t _routine_once_##name = PTHREAD_ONCE_INIT;              \
  static pthread_key_t _routine_key_##name;                                    \
  static int _routine_init_##name = 0;                                         \
  static void _routine_make_key_##name() {                                     \
    (void) pthread_key_create(&_routine_key_##name, NULL);                     \
  }                                                                            \
  template<typename T>                                                         \
  class clsRoutineData_routine_##name {                                        \
  public:                                                                      \
    inline T* operator->() {                                                   \
      if (!_routine_init_##name) {                                             \
        pthread_once(&_routine_once_##name, _routine_make_key_##name);         \
        _routine_init_##name = 1;                                              \
      }                                                                        \
      T* p = (T*) co_getspecific(_routine_key_##name);                         \
      if (!p) {                                                                \
        p = (T*) calloc(1, sizeof(T));                                         \
        int ret = co_setspecific(_routine_key_##name, p);                      \
        if (ret) {                                                             \
          if (p) {                                                             \
            free(p);                                                           \
            p = NULL;                                                          \
          }                                                                    \
        }                                                                      \
      }                                                                        \
      return p;                                                                \
    }                                                                          \
  };                                                                           \
                                                                               \
  static clsRoutineData_routine_##name<name> y;
