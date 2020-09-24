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
# 基址寄存器(存放基地址)包含在小括号"()"中，间接内存引用为"section:disp(base, index, scale)"。
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
# 前者是全局描述符表，位置由GDTR寄存器确定，后者是局部描述符表，位置由LDTR寄存器确定，具体使用哪一个，取决于段选择码
# 中的TI位。CPU 执行指令时在栈里存参数、局部变量等数据
# 栈地址从高到低增长
# 每个函数的栈在 ebp 栈底指针(高地址)和 esp 栈顶指针(低地址)之间，叫做栈帧
# 存在调用关系的两个函数的栈(帧)内存地址是相邻的
# ebp 指针指的位置存储的是上级函数的 ebp 地址，例如 sum 的 ebp 0xffffd598 位置存的是 main 的 ebp0xffffd5c8，
# 目的是sum 执行后可恢复 main 的 ebp，而 main 的 esp 可通过 sum 的 ebp + 4 恢复；这里为啥+4？因为栈的增长方向
# 是从高到低。

# 栈的增长方向是高地址 -> 低地址，配合coctx_swap()参数一起来分析协程上下文切换汇编：
# 当前栈帧顶寄存器ESP指向的地址+4对应的内容，加载到EAX寄存器中，即：下一个函数的栈帧首地址存入EAX存入寄存器中

#if defined(__i386__)
  # save当前协程函数的上下文(寄存器+栈帧)到coctx_swap()参数1中；从coctx_swap()函数的参数2中获取下一个协程函数
  # 的上下文(寄存器+栈帧)，restore到当前寄存器中，并指向该函数新的栈帧。
  # 此时的栈帧是：&(pending_co->ctx) -> &(curr->ctx) -> ret返回地址，且esp寄存器值指向栈顶(返回地址)，
  # 注意：栈的增长方向是由高到低

  # 将coctx_swap()的第一个参数(&(curr->ctx))的地址中存储的数据(curr->ctx)存入eax寄存器中：regs[0] -> eax
  eax寄存器存储第一个参数(当前协程的上下文)，注：mov(指针、地址)指令会解引用，获取其值
  movl 4(%esp), %eax   # regs[0] -> eax
  movl %esp,  28(%eax) # esp -> regs[7]
  movl %ebp, 24(%eax)  # ebp -> regs[6]
  movl %esi, 20(%eax)  # esi -> regs[5]
  movl %edi, 16(%eax)  # edi -> regs[4]
  movl %edx, 12(%eax)  # edx -> regs[3]
  movl %ecx, 8(%eax)   # ecx -> regs[2]
  movl %ebx, 4(%eax)   # ebx -> regs[1]
  # TODO: 要执行的下一条指令地址存储在哪里？即eip寄存器在哪里保存的？？？？？？
  # 答：在call指令调用子函数同时，会将eip寄存器中的值(返回地址、指针)push入栈(子函数的栈帧)，之后把ebp值(父函数
  # 的栈(帧)底部指针)push入栈，再新建子函数的栈帧(父函数esp -> ebp(子函数)，完成子函数栈帧的建立)。

  # 函数调用过程中函数栈的变化过程：
  #【第一步】将参数压入栈中，注意是从右至左的顺序压入。
  #【第二步】调用函数，就是汇编中的call命令，这个命令会同时将当前eip寄存器的值（也就是当前执行的代码地址）压入栈中，
  # 当函数调用完返回的时候，继续取出该代码地址，继续从该点执行。所以也把这个入栈的值叫做返回地址。
  #【第三步】将ebp入栈，也就是保存父函数栈的栈底地址。 然后将esp赋值给ebp，这时ebp等于esp，栈顶等于栈底，也就
  # 是开启了子函数的函数栈。
  #【第四步】局部变量入栈，根据代码处理数据，操作函数栈。
  # 栈: 父函数的之前的栈帧数据->子函数的参数n->子函数的参数1->返回地址(eip寄存器，父函数继续执行的指令地址)
  #     ->ebp(父函数的栈帧底部指针，ebp通常用于定位(取)局部变量)->新的子函数栈帧，
  # 新的子函数栈帧是：局部变量1->局部变量n->等等
  # 子函数执行完毕后，先执行 leave 指令，自动将esp指向的ebp的位置(还原父函数的栈(帧)顶)，并且将之前入栈保存的父函数
  # 的ebp重新赋值给ebp，这样就还原了父函数的栈帧(栈帧是由ebp和esp确定，且由高地址向低地址增长)，然后子函数执行ret
  # 指令，取出之前入栈的返回地址赋值给eip寄存器，父函数就寻址到之前的代码处继续执行。（子函数如果有返回值，通常通过
  # eax返回）；注意：这里的eip存储的下一条指令地址，其实就是父函数调用完子函数返回后，在父函数中的下一条指令地址。
  # 这里父函数与子函数的栈帧是紧凑型的顺序排列，且维系它们栈帧联系的是：保存在父函数栈帧中的父函数返回地址(eip)和父
  # 函数ebp，这样才能还原父函数下一条指令地址、父函数栈帧；
  # 
  # 在分析函数上下文切换过程中，发现上下文的本质是 “一组寄存器” + “函数栈帧”。
  # 通常CPU在从栈帧中取数据(eg: 局部变量)时，使用的是ebp+偏移量(n(%ebp))来get的。且有时候变量入栈(栈帧)，并不是
  # 通过push指令，而是先把esp预分配一块儿栈内容(eg: -24(%esp)，subl $24, %esp)，然后通过ebp+偏移量来入栈。
  # 

  # restore
  # 将coctx_swap()的第二个参数(&(pending_co->ctx))的地址存入eax寄存器
  movl 8(%esp), %eax  # regs[0] -> eax
  movl 4(%eax), %ebx  # regs[1] -> ebx
  movl 8(%eax), %ecx  # regs[2] -> ecx
  movl 12(%eax), %edx # regs[3] -> edx
  movl 16(%eax), %edi # regs[4] -> edi
  movl 20(%eax), %esi # regs[5] -> esi
  movl 24(%eax), %ebp # regs[6] -> ebp # 新的协程函数的栈帧底部
  movl 28(%eax), %esp # regs[7] -> esp # 新的协程函数的栈帧顶部

  ret # ret指令会自动从栈中还原eip寄存器，让其重新指向其父函数的下一条指令地址
  # 此时，寄存器和上下文都切换成了目的协程的了

#elif defined(__x86_64__)
  # %rax 作为函数返回值使用。
  # %rsp 栈指针寄存器，指向栈顶
  # lea与mov区别：lea不解引用，即直接取地址，mov(加括号)是解引用，即：取地址中存储的内容。
  # lea是“load effective address”的缩写，简单的说，lea指令可以用来将一个内存地址直接赋给目的操作数，
  # 例如：lea 8(%ebx), %eax 就是将ebx+8这个值直接赋给eax，而不是把ebx+8处的内存地址里的数据赋给eax。
  # 而mov指令则恰恰相反，例如：mov 8(%ebx), %eax 则是把内存地址为ebx+8处的数据赋给eax

  leaq (%rsp), %rax    # 把rsp寄存器中存储的地址存入rax寄存器，即当前栈帧的栈顶地址加载到RAX寄存器中
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
