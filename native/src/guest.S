/* uint32_t guest_call(gptr fn, int nargs, const uint32_t *args)
   Push args right-to-left and call guest code. Works for stdcall (callee pops) and cdecl
   (caller pops) alike because esp is restored from esi, which guest code preserves. */
        .intel_syntax noprefix
        .text
        .globl  guest_call
        .type   guest_call, @function
guest_call:
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        mov     ecx, [ebp + 12]
        mov     edx, [ebp + 16]
        mov     esi, esp
1:      test    ecx, ecx
        jz      2f
        dec     ecx
        push    dword ptr [edx + ecx * 4]
        jmp     1b
2:      call    dword ptr [ebp + 8]
        mov     esp, esi
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
        .size   guest_call, . - guest_call
        .section .note.GNU-stack, "", @progbits
