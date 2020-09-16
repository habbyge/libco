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
 * 
 * 也就是说，栈帧是在程序栈上按照LIFO方式连续创建的，esp和ebp只能指向程序栈里最顶层的那个栈帧；在代码层面，就是函数的
 * 同步执行过程。如果一个函数发生阻塞，父函数必须等它返回才能继续执行。
 * 这个栈与帧的思路，C++与Java类似，一个线程栈中根据函数调用栈开辟出连续的帧，一个帧对应于一个函数，用于保存函数上下文。
 * 
 * 从上面的讲解可以看出，代码的执行步骤主要受限于唯一的一套寄存器，和LIFO方式的栈帧切换方式。
 * 既然一套寄存器和一块栈帧内存可以决定代码的执行，那么如果把一个正在执行到一半的函数的栈帧和当前寄存器都保存起来，强行把
 * 寄存器赋值成另一个栈帧对应的寄存器值，指向新的栈帧空间，执行新的函数。某一个时刻再以同样的方式切换回原来的函数，从中断
 * 的地方继续执行。这样就可以达到没有规则的在栈帧间跳变，也即使超越代码层面，人为的控制函数的调用关系，而且可以在函数返回
 * 之前提前跳转到别的函数。这样程序是不是可以正常运行？答案当然是可行的。 
 * 这个栈帧之间的切换流程就是协程方案，libco是其中的一种实现。 TODO: 这里继续......
 * 
 * libco有两种栈管理方案：stackless(共享栈模式) and stackfull(独享站模式)。
 * 1、stackless，所有协程共享n块提前分配好的大内存作为栈帧空间
 * 2、stackful，每个创建的协程都会在堆上分配一块默认128k的内存作为自己的栈帧空间
 * 在协程执行过程中，栈帧空间里会存储协程生命周期内所有发生的函数调用生成的栈帧。
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
