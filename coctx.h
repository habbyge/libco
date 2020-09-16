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

#ifndef __CO_CTX_H__
#define __CO_CTX_H__
#include <stdlib.h>

typedef void* (*coctx_pfn_t) (void* s, void* s2);

struct coctx_param_t {
  const void* s1;
  const void* s2;
};

/**
 * 协程上下文：寄存器(体系结构相关) + 栈
 * 程序执行时，CPU通过几个特定的寄存器，包括esp、ebp等，来确定当前执行的代码地址，栈帧的位置等信息，发生函数调用时，
 * 系统会在当前栈帧顶部压入被调用函数的栈帧，存入被调用函数的形参、返回地址等信息（类似Java）；被调用函数执行完返回时，
 * 再回收这个函数的栈帧，返回到上一个栈帧；
 * 也就是说，栈帧是在程序栈上按照LIFO方式连续创建的，esp和ebp只能指向程序栈里最顶层的那个栈帧；在代码层面，就是函数的
 * 同步执行过程。如果一个函数发生阻塞，父函数必须等它返回才能继续执行。
 * 这个栈与帧的思路，C++与Java类似，一个线程栈中根据函数调用栈开辟出连续的帧，一个帧对应于一个函数，用于保存函数上下文。
 * 
 * 从上面的讲解可以看出，代码的执行步骤主要受限于唯一的一套寄存器，和LIFO方式的栈帧切换方式。
 * 既然一套寄存器和一块栈帧内存可以决定代码的执行，那么如果把一个正在执行到一半的函数的栈帧和当前寄存器都保存起来，强行把
 * 寄存器赋值成另一个栈帧对应的寄存器值，指向新的栈帧空间，执行新的函数。某一个时刻 TODO: 这里继续...
 */
struct coctx_t {
#if defined(__i386__)
  void *regs[8];
#else
  void* regs[14];
#endif
  size_t ss_size;
  char* ss_sp;
};

int coctx_init(coctx_t* ctx);
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1);
#endif
