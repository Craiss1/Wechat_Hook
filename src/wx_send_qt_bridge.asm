option casemap:none

EXTERN WxHandleInject:PROC
EXTERN WxHandleRecipient:PROC
EXTERN g_wxInjectTrampoline:QWORD
EXTERN g_wxRecipientTrampoline:QWORD
EXTERN g_wxRecipientContinue:QWORD

PUBLIC WxInjectDetour
PUBLIC WxRecipientDetour

.code

; The injection point is inside the orchestrator and relies on its R13/RBP.
; Preserve every volatile register/XMM register used by the displaced region.
WxInjectDetour PROC
    pushfq
    sub rsp, 0B8h
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    mov [rsp+40h], r10
    mov [rsp+48h], r11
    movdqu xmmword ptr [rsp+50h], xmm0
    movdqu xmmword ptr [rsp+60h], xmm1
    movdqu xmmword ptr [rsp+70h], xmm2
    movdqu xmmword ptr [rsp+80h], xmm3
    movdqu xmmword ptr [rsp+90h], xmm4
    movdqu xmmword ptr [rsp+0A0h], xmm5

    mov rcx, r13
    lea rdx, [rbp+3A0h]
    call WxHandleInject

    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8,  [rsp+30h]
    mov r9,  [rsp+38h]
    mov r10, [rsp+40h]
    mov r11, [rsp+48h]
    movdqu xmm0, xmmword ptr [rsp+50h]
    movdqu xmm1, xmmword ptr [rsp+60h]
    movdqu xmm2, xmmword ptr [rsp+70h]
    movdqu xmm3, xmmword ptr [rsp+80h]
    movdqu xmm4, xmmword ptr [rsp+90h]
    movdqu xmm5, xmmword ptr [rsp+0A0h]
    add rsp, 0B8h
    popfq
    jmp qword ptr [g_wxInjectTrampoline]
WxInjectDetour ENDP

; Return 0 from WxHandleRecipient to run the original submit call.
; Return non-zero to reject and continue after that call.
WxRecipientDetour PROC
    sub rsp, 60h
    mov [rsp+20h], rax
    mov [rsp+28h], rcx
    mov [rsp+30h], rdx
    mov [rsp+38h], r8
    mov [rsp+40h], r9
    mov [rsp+48h], r10
    mov [rsp+50h], r11

    call WxHandleRecipient
    mov [rsp+58h], eax

    mov rax, [rsp+20h]
    mov rcx, [rsp+28h]
    mov rdx, [rsp+30h]
    mov r8,  [rsp+38h]
    mov r9,  [rsp+40h]
    mov r10, [rsp+48h]
    mov r11, [rsp+50h]
    cmp dword ptr [rsp+58h], 0
    jne wxRecipientReject
    add rsp, 60h
    jmp qword ptr [g_wxRecipientTrampoline]

wxRecipientReject:
    add rsp, 60h
    jmp qword ptr [g_wxRecipientContinue]
WxRecipientDetour ENDP

END
