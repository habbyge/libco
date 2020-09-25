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

/**
 * 协程上下文：寄存器(体系结构相关) + 栈(栈内存地址ebp->esp)
 * 程序执行时，CPU通过几个特定的寄存器，包括esp、ebp等，来确定当前执行的代码地址，栈帧的位置等信息，发生函数
 * 调用时，系统会在当前栈帧顶部压入被调用函数的栈帧，存入被调用函数的形参、返回地址等信息（类似Java）；被调用
 * 函数执行完返回时，再回收这个函数的栈帧，返回到上一个栈帧；
 * 
 * 也就是说，栈帧是在程序栈上按照LIFO方式连续创建的，esp和ebp只能指向程序栈里最顶层的那个栈帧；在代码层面，
 * 就是函数的同步执行过程。如果一个函数发生阻塞，父函数必须等它返回才能继续执行。这个栈与帧的思路，C++与
 * Java类似，一个线程栈中根据函数调用栈开辟出连续的帧，一个帧对应于一个函数，用于保存函数上下文。
 * 
 * 从上面的讲解可以看出，代码的执行步骤主要受限于唯一的一套寄存器，和LIFO方式的栈帧切换方式。
 * 既然一套寄存器和一块栈帧内存可以决定代码的执行，那么如果把一个正在执行到一半的函数的栈帧和当前寄存器都保存
 * 起来，强行把寄存器赋值成另一个栈帧对应的寄存器值，指向新的栈帧空间，执行新的函数。某一个时刻再以同样的方式
 * 切换回原来的函数，从中断的地方继续执行。这样就可以达到没有规则的在栈帧间跳变，也即使超越代码层面，人为的控
 * 制函数的调用关系，而且可以在函数返回之前提前跳转到别的函数。这样程序是不是可以正常运行？答案当然是可行的。 
 * 这个栈帧之间的切换流程就是协程方案，libco是其中的一种实现。
 * 
 * libco有两种栈管理方案：stackless(共享栈模式) and stackfull(独享站模式)。
 * 1、stackless，所有协程共享n块提前分配好的大内存作为栈帧空间
 * 2、stackful，每个创建的协程都会在堆上分配一块默认128k的内存作为自己的栈帧空间
 * 在协程执行过程中，栈帧空间里会存储协程生命周期内所有发生的函数调用生成的栈帧。
 * 
 * libco会为进程创建一个默认协程，该协程是第一个执行的，并且初始化的时候没有为regs[1]（保存ESP的变量）设置值。
 * 它执行的是主程序的代码，不会像其他协程那样跳转到用户设置的f函数去执行
 * 
 * 32位使用栈帧来作为传递的参数的保存位置，而64位使用寄存器，分别用rdi,rsi,rdx,rcx,r8,r9作为第1-6个参数，
 * 只有超过6个参数，第七个参数才开始压栈。 所以上面的结论“参数的地址比局部变量的地址高”在64位机器上是不对的，
 * 只有第七个之后的参数才符合"参数的地址比局部变量的地址高"。
 * 
 * // call指令、leave指令：
 * call指令作用是把eip寄存器值(返回地址)压入栈中，并把程序跳转到子函数开头的位置，而ret指令的作用是弹出栈顶
 * 处的父函数中的返回地址给eip寄存器；在函数的ret指令之前，一般会有一个leave指令，这个指令自动让子函数栈帧中
 * 开头保存的父函数的ebp寄存器值(父函数栈帧底部指针)出栈，并还原给ebp寄存器，从而让栈帧切换回父函数；因此上
 * 下文切换时，无需手动保存eip、ebp(？这个待定)、esp(？待定)。
 */

// 定义协程入口函数类型
typedef void* (*coctx_pfn_t) (void* s, void* s2);

struct coctx_param_t {
  const void* s1; // 存放协程入口函数的第1个参数
  const void* s2; // 存放协程入口函数的第2个参数
};

struct coctx_t { // libco中，协程上下文保存在此结构体中
#if defined(__i386__)
  // 32bit协程上下文寄存器：  
  // low  | regs[0]: ret |
  //      | regs[1]: ebx |
  //      | regs[2]: ecx |
  //      | regs[3]: edx |
  //      | regs[4]: edi |
  //      | regs[5]: esi |
  //      | regs[6]: ebp |
  // high | regs[7]: eax |  = esp
  void* regs[8]; // 8个寄存器
#else
  void* regs[14]; // 最多存储14个寄存器，每个寄存器是8字节，所以寄存器组最后一个偏移量是112=13*8
#endif
  size_t ss_size; // 栈大小
  char* ss_sp;    // 协程栈底：每个协程都有独立的栈控件(栈帧)，sp+size表示栈顶指针
};

int coctx_init(coctx_t* ctx);
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1);
#endif
