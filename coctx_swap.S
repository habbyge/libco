/*
* Tencent is pleased to support the open source community by making Libco available.

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

# GNU汇编采用 AT&T 语法，所以其中的源和目的操作数和 Intel 文档中给出的顺序是相反的，即：
# movx source, destination
# 立即数的语法是：$立即数，例如: movl $2, $edi 把32bit立即数2，传递给寄存器EDI
# 寄存器名称有 "%" 前缀，即如果必须使用 "eax"，它应该用作 "%eax"
# 立即数以 "$" 为前缀。静态 "C" 变量也使用 "$" 前缀。16进制常量使用前缀 "0x"。所以，对
# 于十六进制，我们首先看到一个 "$"，然后是 "0x"，最后才是常量。
# 操作码后缀 ’b’ 、’w’、’l’ 分别指明了字节（8位）、字（16位）、长型（32位）存储器引用。
# 基址寄存器包含在小括号"()"中，间接内存引用为"section:disp(base, index, scale)"。
# 

.globl coctx_swap
#if !defined( __APPLE__ )
.type  coctx_swap, @function
#endif
coctx_swap: # 存储当前协程函数的寄存器到内存中；加载下一个协程函数的寄存器到当前寄存器组中

# i386中经常使用的16个寄存器：
# 1.通用寄存器（8个）
# - EAX、EBX、ECX、EDX这四个寄存器，主要就是用来暂时存放计算过程中所用的操作数、结果或其它信息
# - ESP为堆栈指针寄存，它和堆栈段寄存器SS共同决定了当前的栈指针，每当执行push,pull操作时，或者因为某些原因（如中断），
# CPU自动将数据入栈时，就会使用该指针来找到堆栈栈顶（或栈底）的位置，然后执行压栈、出栈的操作。系统管理软件，如操作系
# 统会根据其分配的栈空间地址来设定这两个寄存器的值
# - EBP称为基址指针寄存器，它和ESP都可以与堆栈段寄存器SS联用来确定堆栈中的某一存储单元的地址，ESP用来指示段顶的偏移地
# 址，而EBP可作为堆栈区中的一个基地址以便访问堆栈中的信息
# - ESI（源变址寄存器）和EDI（目的变址寄存器）一般与数据段寄存器DS联用，用来确定数据段中某一存储单元的地址。这两个变址
# 寄存器有自动增量和自动减量的功能，可以很方便地用于变址。在串处理指令中，ESI和EDI作为隐含的源变址和目的变址寄存器时，
# ESI和DS联用，EDI和附加段ES联用，分别达到在数据段和附加段中寻址的目的
# 2.专用寄存器
# EIP指令指针寄存器，它用来存放代码段中的偏移地址。在程序运行的过程中，它始终指向下一条指令的首地址。它与段寄存器CS联用确
# 定下一条指令的物理地址。当这一地址送到存储器后，控制器可以取得下一条要执行的指令，而控制器一旦取得这条指令就马上修改EIP
# 的内容，使它始终指向下一条指令的首地址。那些跳转指令，就是通过修改EIP的值来达到相应的目的的。
# FLAGS标志寄存器，又称PSW(program status word)，即程序状态寄存器。这一个是存放条件标志码、控制标志和系统标志的寄存器。
# 3.段寄存器
# 一共六个，分别是CS代码段，DS数据段，ES附加段，SS堆栈段，FS以及GS这两个还是附加段。
# EFLAGS寄存器中的IF位表示是否允许中断，为1允许，否则不允许。
# TR寄存器
# 用来指向当前任务的TSS段
# IDTR寄存器
# 用来指向当前IDT（中断表述符表或者说是中断向量表），因为在保护模式下，IDT的起始地址可以在任何位置，而不仅限于地址0。
# GDT和LDT
# 前者是全局描述符表，位置由GDTR寄存器确定，后者是局部描述符表，位置由LDTR寄存器确定，具体使用哪一个，取决于段选择码中的TI位。
#if defined(__i386__)
  # 当前栈帧顶寄存器ESP指向的地址+4对应的内容，加载到EAX寄存器中，即：下一个函数的栈帧首地址存入EAX存入寄存器中
  movl 4(%esp), %eax    
  movl %esp,  28(%eax)  # 栈顶寄存器ESP，加载到EAX中存储的地址(ESP+4+28地址处) 中
  movl %ebp, 24(%eax)   # 当前栈帧底部地址寄存器所存储的当前栈帧基地址，加载到EAX寄存器中存储的地址(ESP+4+24的地址处)中
  movl %esi, 20(%eax)   # TODO: continue here !!!
  movl %edi, 16(%eax)
  movl %edx, 12(%eax)
  movl %ecx, 8(%eax)    # ECX寄存器中的值，加载到当前函数栈帧地址+8位置处
  movl %ebx, 4(%eax)    # EBX寄存器值，加载到 EAX中存储的地址+4字节的位置，即调用者函数栈帧的下一个地址(指针)处

  movl 8(%esp), %eax
  movl 4(%eax), %ebx
  movl 8(%eax), %ecx
  movl 12(%eax), %edx
  movl 16(%eax), %edi
  movl 20(%eax), %esi
  movl 24(%eax), %ebp
  movl 28(%eax), %esp

  ret

#elif defined(__x86_64__)
# %rax 作为函数返回值使用。
# %rsp 栈指针寄存器，指向栈顶
  leaq (%rsp), %rax    # 把RSP中存储的地址存入RAX寄存器，即当前栈帧的栈顶地址加载到RAX寄存器中
  movq %rax, 104(%rdi)
  movq %rbx, 96(%rdi)
  movq %rcx, 88(%rdi)
  movq %rdx, 80(%rdi)
  movq 0(%rax), %rax
  movq %rax, 72(%rdi) 
  movq %rsi, 64(%rdi)
  movq %rdi, 56(%rdi)
  movq %rbp, 48(%rdi)
  movq %r8, 40(%rdi)
  movq %r9, 32(%rdi)
  movq %r12, 24(%rdi)
  movq %r13, 16(%rdi)
  movq %r14, 8(%rdi)
  movq %r15, (%rdi)
  xorq %rax, %rax

  movq 48(%rsi), %rbp
  movq 104(%rsi), %rsp
  movq (%rsi), %r15
  movq 8(%rsi), %r14
  movq 16(%rsi), %r13
  movq 24(%rsi), %r12
  movq 32(%rsi), %r9
  movq 40(%rsi), %r8
  movq 56(%rsi), %rdi
  movq 80(%rsi), %rdx
  movq 88(%rsi), %rcx
  movq 96(%rsi), %rbx
  leaq 8(%rsp), %rsp
  pushq 72(%rsi)

  movq 64(%rsi), %rsi
  ret
#endif
