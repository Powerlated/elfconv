.text
.p2align 4
.globl local_dispatch
.type local_dispatch,@function
local_dispatch:
.cfi_startproc
  movl 4(%esp), %eax
  movl .Ltargets(,%eax,4), %eax
  jmp *%eax
.Lsubtract:
  movl 8(%esp), %eax
  subl $11, %eax
  ret
.Lmultiply:
  movl 8(%esp), %eax
  imull $5, %eax
  ret
.cfi_endproc
.size local_dispatch, .-local_dispatch

.p2align 4
.globl tail_dispatch
.type tail_dispatch,@function
tail_dispatch:
.cfi_startproc
  movl 8(%esp), %eax
  jmp *%eax
.cfi_endproc
.size tail_dispatch, .-tail_dispatch

# This helper has an unwind boundary but no symbol-table entry. Its indirect
# branch must retain its local target without enumerating the later padding.
.p2align 4
.Lunwind_helper:
.cfi_startproc
  movl 4(%esp), %eax
  addl $7, %eax
  movl $.Lunwind_return, %edx
  jmp *%edx
.Lunwind_return:
  ret
.cfi_endproc

.space 4096, 0x90
.p2align 4
.globl unwind_dispatch
.type unwind_dispatch,@function
unwind_dispatch:
.cfi_startproc
  jmp .Lunwind_helper
.cfi_endproc
.size unwind_dispatch, .-unwind_dispatch

.section .rodata
.p2align 2
.Ltargets:
  .long .Lsubtract, .Lmultiply
.section .note.GNU-stack,"",@progbits
