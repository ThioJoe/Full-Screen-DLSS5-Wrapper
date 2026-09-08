; _penter/_pexit for MSVC /Gh /GH: preserve every volatile register, realign the stack, then call
; TraceEnter/TraceExit with the instrumented function's address (the hook's return address) in rcx.
; The hooks run before the prologue and after the epilogue, so rsp arrives 16-byte aligned, unlike a
; normal call; rbp keeps the frame and rsp is aligned explicitly, which holds for either entry state.
EXTERN TraceEnter:PROC
EXTERN TraceExit:PROC

.code

_penter PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov rbp, rsp
    sub rsp, 128
    and rsp, -16
    movdqu xmmword ptr [rbp - 16], xmm0
    movdqu xmmword ptr [rbp - 32], xmm1
    movdqu xmmword ptr [rbp - 48], xmm2
    movdqu xmmword ptr [rbp - 64], xmm3
    movdqu xmmword ptr [rbp - 80], xmm4
    movdqu xmmword ptr [rbp - 96], xmm5
    mov rcx, qword ptr [rbp + 64]
    call TraceEnter
    movdqu xmm5, xmmword ptr [rbp - 96]
    movdqu xmm4, xmmword ptr [rbp - 80]
    movdqu xmm3, xmmword ptr [rbp - 64]
    movdqu xmm2, xmmword ptr [rbp - 48]
    movdqu xmm1, xmmword ptr [rbp - 32]
    movdqu xmm0, xmmword ptr [rbp - 16]
    mov rsp, rbp
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    ret
_penter ENDP

_pexit PROC
    push rax
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbp
    mov rbp, rsp
    sub rsp, 128
    and rsp, -16
    movdqu xmmword ptr [rbp - 16], xmm0
    movdqu xmmword ptr [rbp - 32], xmm1
    movdqu xmmword ptr [rbp - 48], xmm2
    movdqu xmmword ptr [rbp - 64], xmm3
    movdqu xmmword ptr [rbp - 80], xmm4
    movdqu xmmword ptr [rbp - 96], xmm5
    mov rcx, qword ptr [rbp + 64]
    call TraceExit
    movdqu xmm5, xmmword ptr [rbp - 96]
    movdqu xmm4, xmmword ptr [rbp - 80]
    movdqu xmm3, xmmword ptr [rbp - 64]
    movdqu xmm2, xmmword ptr [rbp - 48]
    movdqu xmm1, xmmword ptr [rbp - 32]
    movdqu xmm0, xmmword ptr [rbp - 16]
    mov rsp, rbp
    pop rbp
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    pop rax
    ret
_pexit ENDP

END
