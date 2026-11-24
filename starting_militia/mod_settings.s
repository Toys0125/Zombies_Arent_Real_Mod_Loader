.globl layout_mod_settings, layout_mod_settings_end
.globl draw_mod_settings, draw_mod_settings_end
.globl action_mod_settings, action_mod_settings_end
.globl input_mod_settings, input_mod_settings_end

.equ MODS_TAB, 9
.equ FIRST_MODS_ACTION, 200
.equ LAST_MODS_ACTION, 208

# The stock layout has built its navigation rows and the Back button.
layout_mod_settings:
.ifdef STEAM_V044
    mov rcx, qword ptr [rsp + 0x38]
    movaps xmm1, xmm14
.else
    mov rcx, qword ptr [rsp + 0x40]
    movss xmm1, dword ptr [rsp + 0x3c]
.endif
    call build_mod_settings_widgets
    mov r14, qword ptr [rip + settings_widgets + 8]
    movzx eax, byte ptr [rip + settings_tab]
    cmp eax, MODS_TAB
    je settings_layout_finish
    jmp settings_layout_dispatch
layout_mod_settings_end:

# Stock rendering already drew every widget. Add the Mods page's labels
# and then resume the common Settings footer.
draw_mod_settings:
    movzx eax, byte ptr [rip + settings_tab]
    cmp eax, MODS_TAB
    jne settings_draw_dispatch
    mov rcx, qword ptr [rsp + 0x50]
    call draw_mod_settings_labels
    jmp settings_draw_finish
draw_mod_settings_end:

action_mod_settings:
    cmp cl, FIRST_MODS_ACTION
    jb .stock_setting_action
    cmp cl, LAST_MODS_ACTION
    ja .stock_setting_action
    cmp byte ptr [rip + settings_visible], 0
    je .stock_setting_action
    movzx ecx, cl
    call handle_mod_setting_action
    jmp settings_action_finish
.stock_setting_action:
    movzx edi, cl
    lea eax, [rdi - 1]
    jmp settings_action_dispatch
action_mod_settings_end:

# Preserve the window procedure's existing input handling everywhere
# outside an active Mods text field.
input_mod_settings:
    cmp byte ptr [rip + settings_tab], MODS_TAB
    jne .stock_window_message
    cmp edx, 0x100
    jb .stock_window_message
    cmp edx, 0x102
    ja .stock_window_message
    mov ecx, edx
    mov edx, r14d
    call handle_mod_setting_message
    test eax, eax
    jne settings_input_handled
    mov rcx, r12
    mov edx, ebp
    mov r8, r14
    mov r9, rsi
.stock_window_message:
    cmp edx, 0x112
    jmp settings_input_dispatch
input_mod_settings_end:
