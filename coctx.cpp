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

/**
 * 基础知识：
 * 栈帧，每个栈帧都对应着一个未运行完的函数，栈帧中保存了该函数的函数参数，返回地址和局部变量等数据，类似Java。
 */

#include "coctx.h"
#include <stdio.h>
#include <string.h>

#define ESP 0 // 栈指针寄存器(Extended Stack Pointer)，其内存放着一个指针，该指针永远指向当前栈帧的栈顶
#define EIP 1
#define EAX 2
#define ECX 3
// ----------- x86-64
#define RSP 0 // 表示栈顶寄存器
#define RIP 1 // 指向汇编代码栈的指令地址，表示即将要执行的指令
#define RBX 2 // 为函数返回值的存储寄存器
#define RDI 3 // 通常存储函数的第1个参数地址
#define RSI 4 // 通常存储函数的第2个参数地址

#define RBP 5 // 表示栈底寄存器
#define R12 6
#define R13 7
#define R14 8
#define R15 9
#define RDX 10
#define RCX 11
#define R8 12
#define R9 13

//----- --------
// 32 bit
// | regs[0]: ret |
// | regs[1]: ebx |
// | regs[2]: ecx |
// | regs[3]: edx |
// | regs[4]: edi |
// | regs[5]: esi |
// | regs[6]: ebp |
// | regs[7]: eax |  = esp
enum {
  kEIP = 0,
  kEBP = 6, // EBP: 基地址指针寄存器(Extended Base Pointer)，其内存放着一个指针，该指针永远指向当前栈帧的底部
  kESP = 7,
};

//-------------
// 64 bit
// low | regs[0]: r15 |
//    | regs[1]: r14 |
//    | regs[2]: r13 |
//    | regs[3]: r12 |
//    | regs[4]: r9  |
//    | regs[5]: r8  |
//    | regs[6]: rbp |
//    | regs[7]: rdi | // 通常存储函数的第1个参数地址
//    | regs[8]: rsi | // 通常存储函数的第2个参数地址
//    | regs[9]: ret |  //ret func addr
//    | regs[10]: rdx |
//    | regs[11]: rcx |
//    | regs[12]: rbx |
// hig | regs[13]: rsp |
enum {
  kRDI = 7,
  kRSI = 8,
  kRETAddr = 9,
  kRSP = 13,
};

// 64 bit
extern "C" {
  extern void coctx_swap(coctx_t*, coctx_t*) asm("coctx_swap");
};
#if defined(__i386__)
int coctx_init(coctx_t* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  return 0;
}
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {
  // make room for coctx_param
  char* sp = ctx->ss_sp + ctx->ss_size - sizeof(coctx_param_t);
  sp = (char*) ((unsigned long) sp & -16L);

  coctx_param_t* param = (coctx_param_t*) sp;
  void** ret_addr = (void**) (sp - sizeof(void*) * 2);
  *ret_addr = (void*) pfn;
  param->s1 = s;
  param->s2 = s1;

  memset(ctx->regs, 0, sizeof(ctx->regs));

  ctx->regs[kESP] = (char*) (sp) - sizeof(void*) * 2;
  return 0;
}
#elif defined(__x86_64__)
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1) {
  char* sp = ctx->ss_sp + ctx->ss_size - sizeof(void*);
  sp = (char*) ((unsigned long) sp & -16LL);

  memset(ctx->regs, 0, sizeof(ctx->regs));
  void** ret_addr = (void**) (sp);
  *ret_addr = (void*) pfn;

  ctx->regs[kRSP] = sp; // 栈顶寄存器，存储在第13索引处(寄存器数组中的最后一个位置)
  ctx->regs[kRETAddr] = (char*) pfn; // 函数返回地址存储在第9索引处
  ctx->regs[kRDI] = (char*) s; // 通常存储函数的第1个参数地址
  ctx->regs[kRSI] = (char*) s1; // 通常存储函数的第2个参数地址
  return 0;
}

int coctx_init(coctx_t* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  return 0;
}

#endif
