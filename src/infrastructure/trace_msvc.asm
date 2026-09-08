; _penter/_pexit for MSVC /Gh /GH: preserve every volatile register, then call TraceEnter/TraceExit
; with the instrumented function's address (the hook's return address) in rcx.
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
    sub rsp, 128
    movdqu xmmword ptr [rsp + 32], xmm0
    movdqu xmmword ptr [rsp + 48], xmm1
    movdqu xmmword ptr [rsp + 64], xmm2
    movdqu xmmword ptr [rsp + 80], xmm3
    movdqu xmmword ptr [rsp + 96], xmm4
    movdqu xmmword ptr [rsp + 112], xmm5
    mov rcx, qword ptr [rsp + 184]
    call TraceEnter
    movdqu xmm5, xmmword ptr [rsp + 112]
    movdqu xmm4, xmmword ptr [rsp + 96]
    movdqu xmm3, xmmword ptr [rsp + 80]
    movdqu xmm2, xmmword ptr [rsp + 64]
    movdqu xmm1, xmmword ptr [rsp + 48]
    movdqu xmm0, xmmword ptr [rsp + 32]
    add rsp, 128
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
    sub rsp, 128
    movdqu xmmword ptr [rsp + 32], xmm0
    movdqu xmmword ptr [rsp + 48], xmm1
    movdqu xmmword ptr [rsp + 64], xmm2
    movdqu xmmword ptr [rsp + 80], xmm3
    movdqu xmmword ptr [rsp + 96], xmm4
    movdqu xmmword ptr [rsp + 112], xmm5
    mov rcx, qword ptr [rsp + 184]
    call TraceExit
    movdqu xmm5, xmmword ptr [rsp + 112]
    movdqu xmm4, xmmword ptr [rsp + 96]
    movdqu xmm3, xmmword ptr [rsp + 80]
    movdqu xmm2, xmmword ptr [rsp + 64]
    movdqu xmm1, xmmword ptr [rsp + 48]
    movdqu xmm0, xmmword ptr [rsp + 32]
    add rsp, 128
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
