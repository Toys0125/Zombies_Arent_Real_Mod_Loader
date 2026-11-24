.globl add_building_types_button, add_building_types_button_end
.globl append_battle_view_button, append_battle_view_button_end
.globl hit_test_battle_view_button
.ifndef STEAM_V044
.globl draw_three_view_buttons, draw_three_view_buttons_end
.globl lower_click_paint_rows, lower_click_paint_rows_end
.endif
.globl apply_building_type_colors, apply_building_type_colors_end
.globl prefer_building_type_color, prefer_building_type_color_end
.globl statpanel_building_use, statpanel_building_use_end
.globl draw_building_type_key_panel, draw_building_type_key_panel_end
.globl draw_building_type_key_text, draw_building_type_key_text_end
.globl draw_building_type_key, draw_building_type_key_end

# v0.44.0 uses view 5 for Ownership.
.ifdef STEAM_V044
.equ BUILDING_TYPES_VIEW, 6
.else
.equ BUILDING_TYPES_VIEW, 5
.endif
.equ VIEW_BUTTON_GROUP, 9
.equ BUILDING_USE_COUNT, 14
.equ INFERRED_HOME_COLOR, 14
.equ BUILDING_KEY_COUNT, 16
.equ SPAWN_INFO_ROWS, 5
.equ LAST_INFERRED_USE, 6

add_building_types_button:
.ifdef STEAM_V044
    # The stock slice holds Classes, Height, and Ownership in co-op.
    # Copy its entries before appending ours so the original stack stays intact.
    lea rdx, [rip + view_button_scratch]
    imul r8, rcx, 0x58
    xor r9d, r9d
.copy_view_buttons:
    mov r10, qword ptr [rax + r9]
    mov qword ptr [rdx + r9], r10
    add r9, 8
    cmp r9, r8
    jb .copy_view_buttons
    lea r8, [rdx + r8]
    pxor xmm0, xmm0
    movups xmmword ptr [r8], xmm0
    lea rax, [rip + building_types_label]
    mov qword ptr [r8 + 0x10], rax
    mov qword ptr [r8 + 0x18], 14
    movups xmmword ptr [r8 + 0x20], xmm0
    movups xmmword ptr [r8 + 0x30], xmm0
    movups xmmword ptr [r8 + 0x40], xmm0
    mov byte ptr [r8 + 0x41], VIEW_BUTTON_GROUP
    mov qword ptr [r8 + 0x50], 0x100 + BUILDING_TYPES_VIEW
    cmp byte ptr [rip + active_view], BUILDING_TYPES_VIEW
    sete byte ptr [r8 + 0x52]
    inc rcx
    mov qword ptr [rsp + 0x120], rdx
    mov qword ptr [rsp + 0x50], rcx
    mov qword ptr [rsp + 0x128], rcx
.else
    # The stock toolbar assembles Classes and Height at rsp+0x290.
    # The next entry fits in scratch space used earlier for another group.
    pxor xmm0, xmm0
    movups xmmword ptr [rdi + 0xb0], xmm0
    lea rax, [rip + building_types_label]
    mov qword ptr [rdi + 0xc0], rax
    mov qword ptr [rdi + 0xc8], 14
    movups xmmword ptr [rdi + 0xd0], xmm0
    movups xmmword ptr [rdi + 0xe0], xmm0
    movups xmmword ptr [rdi + 0xf0], xmm0
    mov byte ptr [rdi + 0xf1], VIEW_BUTTON_GROUP
    mov qword ptr [rdi + 0x100], 0x100 + BUILDING_TYPES_VIEW
    cmp byte ptr [rip + active_view], BUILDING_TYPES_VIEW
    sete byte ptr [rdi + 0x102]
    mov qword ptr [rsp + 0x108], 3
.endif
    jmp measure_view_buttons
add_building_types_button_end:

# The last element is the stock Restart button. Use its bottom and left edges
# to position a normal View button, and grow the game's array if it is full.
append_battle_view_button:
    inc qword ptr [rip + toolbar_element_count]
    mov rax, qword ptr [rip + toolbar_element_count]
    mov r9, qword ptr [rip + toolbar_capacity]
    cmp rax, r9
    jl .append_battle_view
    add r9, r9
    lea rdx, [rax + 1]
    cmp r9, rdx
    cmovl r9, rdx
    lea rcx, [rip + toolbar_elements]
    mov edx, 0x70
    mov r8d, 8
    lea rax, [rip + restart_layout_site]
    mov qword ptr [rsp + 0x20], rax
    mov rax, qword ptr [rsp + 0x40]
    mov qword ptr [rsp + 0x28], rax
    call grow_toolbar
    mov rax, qword ptr [rip + toolbar_element_count]
    cmp rax, qword ptr [rip + toolbar_capacity]
    jge finish_restart_layout
.append_battle_view:
    imul rax, rax, 0x70
    add rax, qword ptr [rip + toolbar_elements]
    movss xmm0, dword ptr [rax - 0x70]
    subss xmm0, dword ptr [rip + battle_button_gap]
    movss dword ptr [rax + 8], xmm0
    subss xmm0, dword ptr [rip + battle_button_width]
    movss dword ptr [rax], xmm0
    movss xmm0, dword ptr [rax - 0x70 + 0xc]
    movss dword ptr [rax + 0xc], xmm0
    subss xmm0, dword ptr [rip + battle_button_height]
    movss dword ptr [rax + 4], xmm0
    pxor xmm0, xmm0
    movups xmmword ptr [rax + 0x10], xmm0
    lea rcx, [rip + building_types_label]
    mov qword ptr [rax + 0x20], rcx
    mov qword ptr [rax + 0x28], 14
    movups xmmword ptr [rax + 0x30], xmm0
    movups xmmword ptr [rax + 0x40], xmm0
    movups xmmword ptr [rax + 0x50], xmm0
    movups xmmword ptr [rax + 0x60], xmm0
    mov byte ptr [rax + 0x59], VIEW_BUTTON_GROUP
    mov word ptr [rax + 0x68], 0x100 + BUILDING_TYPES_VIEW
    cmp byte ptr [rip + active_view], BUILDING_TYPES_VIEW
    sete byte ptr [rax + 0x6a]
    mov byte ptr [rax + 0x6c], 1
    inc qword ptr [rip + toolbar_element_count]
    jmp finish_restart_layout
append_battle_view_button_end:

# Keep the stock HUD regions, then check the battle button's own bounds.
# The stock hit test leaves the cursor as (x, y, x, y) in xmm0.
hit_test_battle_view_button:
    cmp eax, 0xf
    sete al
    and al, cl
    jnz .hit_test_done
    cmp qword ptr [rip + battle_phase], 1
    jne .hit_test_done
    mov rcx, qword ptr [rip + toolbar_element_count]
    test rcx, rcx
    jle .hit_test_done
    mov rdx, qword ptr [rip + toolbar_elements]
.hit_test_next:
    cmp byte ptr [rdx + 0x59], VIEW_BUTTON_GROUP
    jne .hit_test_advance
    cmp word ptr [rdx + 0x68], 0x100 + BUILDING_TYPES_VIEW
    jne .hit_test_advance
    cmp byte ptr [rdx + 0x6b], 0
    jne .hit_test_advance
    movups xmm1, xmmword ptr [rdx]
    movaps xmm2, xmm0
    cmpleps xmm2, xmm1
    cmpleps xmm1, xmm0
    blendps xmm1, xmm2, 0xc
    movmskps ecx, xmm1
    cmp ecx, 0xf
    sete al
    ret
.hit_test_advance:
    add rdx, 0x70
    dec rcx
    jne .hit_test_next
.hit_test_done:
    ret

.ifndef STEAM_V044
draw_three_view_buttons:
    mov qword ptr [rsp + 0xc8], 3
    jmp draw_view_buttons
draw_three_view_buttons_end:

lower_click_paint_rows:
    addss xmm8, dword ptr [rip + click_paint_offset]
    addss xmm8, dword ptr [rip + toolbar_row_height]
    jmp position_click_paint
lower_click_paint_rows_end:
.endif

apply_building_type_colors:
    movzx eax, byte ptr [rip + active_view]
    cmp al, BUILDING_TYPES_VIEW
    jne stock_color_mode

    # Run after the game's default color pass. Leave destroyed buildings at
    # their stock colors, and use Unknown for absent or invalid use data.
    mov rcx, qword ptr [rip + building_count]
    cmp rsi, rcx
    cmovg rsi, rcx
    test rsi, rsi
    jle finish_building_colors
    call refresh_inferred_building_uses
    xor ebx, ebx
    cmp rsi, qword ptr [rip + inferred_building_count]
    cmovbe rbx, qword ptr [rip + inferred_building_uses]
    mov r8, qword ptr [rip + building_colors]
    mov r9, qword ptr [rip + building_attributes]
    mov r10, qword ptr [rip + building_attribute_count]
    mov r11, qword ptr [rip + destroyed_buildings]
    mov rbp, qword ptr [rip + destroyed_building_count]
    lea rdi, [rip + building_type_palette]
    xor ecx, ecx
.type_color_next:
    cmp rcx, rbp
    jge .type_color_lookup
    cmp byte ptr [r11 + rcx], 0
    jne .type_color_advance
.type_color_lookup:
    xor edx, edx
    cmp rcx, r10
    jge .type_color_inferred
    movzx eax, byte ptr [r9 + rcx*4 + 3]
    test eax, eax
    je .type_color_inferred
    cmp eax, BUILDING_USE_COUNT
    cmovb edx, eax
    jmp .type_color_store
.type_color_inferred:
    test rbx, rbx
    je .type_color_store
    movzx eax, byte ptr [rbx + rcx]
    and eax, INFERRED_USE_MASK
    je .type_color_store
    lea edx, [rax + INFERRED_HOME_COLOR - 1]
.type_color_store:
    mov eax, dword ptr [rdi + rdx*4]
    mov dword ptr [r8 + rcx*4], eax
.type_color_advance:
    inc rcx
    cmp rcx, rsi
    jl .type_color_next
    jmp finish_building_colors
apply_building_type_colors_end:

prefer_building_type_color:
    # Keep the type visible on roofs that ordinarily take a unit color.
.ifdef STEAM_V044
    movzx r8d, byte ptr [rip + active_view]
.endif
    cmp r8d, BUILDING_TYPES_VIEW
    je shade_building_color
.ifndef STEAM_V044
    cmp r8d, 4  # The existing placement overlay also skips unit colors.
    je shade_building_color
.endif
    jmp stock_building_color
prefer_building_type_color_end:

statpanel_building_use:
    # r13 identifies the building, rbx is the panel and rdi is the Odin context.
    xor ecx, ecx
    cmp r13, qword ptr [rip + building_attribute_count]
    jae .save_height
    mov rax, qword ptr [rip + building_attributes]
    cmp byte ptr [rax + 4*r13 + 3], 0
    jne stock_statpanel_building_use
    movzx ecx, word ptr [rax + 4*r13]
.save_height:
    mov dword ptr [rsp + 0x4c], ebp
    mov word ptr [rsp + 0x68], cx
    call refresh_inferred_building_uses
    cmp r13, qword ptr [rip + inferred_building_count]
    jae statpanel_building_height
    mov rax, qword ptr [rip + inferred_building_uses]
    test rax, rax
    je statpanel_building_height
    movzx eax, byte ptr [rax + r13]
    cmp eax, LAST_INFERRED_USE
    ja statpanel_building_height
    imul eax, eax, 24
    lea rcx, [rip + statpanel_use_names]
    add rcx, rax
    movzx edx, byte ptr [rcx]
    test edx, edx
    je statpanel_building_height
    mov esi, edx
    lea rbp, [rcx + 1]
    # The game's Use row retains the supplied string when there is no translation key.
    jmp statpanel_translate_building_use
statpanel_building_use_end:

# The native height key has separate geometry and text passes. Draw the
# building-use key at the equivalent points in each pass.
draw_building_type_key_panel:
    jne draw_height_key_panel
    cmp byte ptr [rip + active_view], BUILDING_TYPES_VIEW
    jne after_height_key_panel
.ifdef STEAM_V044
    mov rcx, r12
.else
    mov rcx, rsi
.endif
    xor edx, edx
    call draw_building_type_key
    jmp after_height_key_panel
draw_building_type_key_panel_end:

draw_building_type_key_text:
    jne draw_height_key_text
    cmp byte ptr [rip + active_view], BUILDING_TYPES_VIEW
    jne after_height_key_text
.ifdef STEAM_V044
    # The text pass keeps its Odin context in r12 and the view ID in esi.
    mov rcx, r12
.else
    mov rcx, rsi
.endif
    mov edx, 1
    call draw_building_type_key
    jmp after_height_key_text
draw_building_type_key_text_end:

draw_building_type_key:
    push rbx
    sub rsp, 0x90
    mov qword ptr [rsp + 0x30], rcx  # Odin context.
    mov dword ptr [rsp + 0x38], edx  # Zero for shapes, one for text.
    cmp qword ptr [rip + battle_phase], 0
    jne .save_info_visibility
    call update_spawn_estimate
.save_info_visibility:
    movzx eax, byte ptr [rip + militia_spawn_info_ready]
    mov dword ptr [rsp + 0x84], eax

    # The planning key sits above the View group. Keep the battle key clear
    # of the Restart and speed controls.
    cmp qword ptr [rip + battle_phase], 0
    jne .battle_key
    mov rcx, qword ptr [rip + toolbar_element_count]
    test rcx, rcx
    jle .key_done
    mov rax, qword ptr [rip + toolbar_elements]
.find_view_button:
    cmp word ptr [rax + 0x68], 0x100 + BUILDING_TYPES_VIEW
    jne .next_view_button
    cmp byte ptr [rax + 0x59], VIEW_BUTTON_GROUP
    je .position_key
.next_view_button:
    add rax, 0x70
    dec rcx
    jne .find_view_button
    jmp .key_done

.position_key:
    movss xmm0, dword ptr [rax + 8]
    subss xmm0, dword ptr [rip + key_width]
    movss xmm1, dword ptr [rax + 4]
    subss xmm1, dword ptr [rip + key_top_offset]
    cmp dword ptr [rsp + 0x84], 0
    je .clamp_key_position
    subss xmm1, dword ptr [rip + spawn_info_height]
.clamp_key_position:
    maxss xmm0, dword ptr [rip + key_margin]
    maxss xmm1, dword ptr [rip + key_margin]
    jmp .draw_key
.battle_key:
    movss xmm0, dword ptr [rip + key_margin]
    movaps xmm1, xmm0
.draw_key:
    movss dword ptr [rsp + 0x40], xmm0
    movss dword ptr [rsp + 0x44], xmm1
    cmp dword ptr [rsp + 0x38], 0
    jne .prepare_key_text

    movaps xmm2, xmm0
    addss xmm2, dword ptr [rip + key_width]
    movaps xmm3, xmm1
    addss xmm3, dword ptr [rip + key_height]
    cmp dword ptr [rsp + 0x84], 0
    je .draw_key_border
    addss xmm3, dword ptr [rip + spawn_info_height]
.draw_key_border:
    mov rcx, qword ptr [rsp + 0x30]
    mov qword ptr [rsp + 0x28], rcx
    mov dword ptr [rsp + 0x20], 0xff6c6152
    call draw_ui_rectangle

    movss xmm0, dword ptr [rsp + 0x40]
    movss xmm1, dword ptr [rsp + 0x44]
    addss xmm0, dword ptr [rip + key_border]
    addss xmm1, dword ptr [rip + key_border]
    movaps xmm2, xmm0
    movaps xmm3, xmm1
    addss xmm2, dword ptr [rip + key_inner_width]
    addss xmm3, dword ptr [rip + key_inner_height]
    cmp dword ptr [rsp + 0x84], 0
    je .draw_key_background
    addss xmm3, dword ptr [rip + spawn_info_height]
.draw_key_background:
    mov rcx, qword ptr [rsp + 0x30]
    mov qword ptr [rsp + 0x28], rcx
    mov dword ptr [rsp + 0x20], 0xff261e19
    call draw_ui_rectangle
    jmp .draw_key_entries

.prepare_key_text:
    mov rax, qword ptr [rip + ui_text_position]
    mov qword ptr [rsp + 0x78], rax
    mov eax, dword ptr [rip + ui_text_color]
    mov dword ptr [rsp + 0x80], eax
    mov dword ptr [rip + ui_text_color], 0xffe2dcdc
    mov ecx, 220
    mov edx, 220
    mov r8d, 226
    mov r9d, 255
    call set_text_rgba
    lea rax, [rsp + 0x48]
    mov qword ptr [rsp + 0x58], rax
    movabs rax, 0x0e285d333f62babf  # Native string type for the %s formatter.
    mov qword ptr [rsp + 0x60], rax
    lea rax, [rsp + 0x58]
    mov qword ptr [rsp + 0x68], rax
    mov qword ptr [rsp + 0x70], 1

.draw_key_entries:
    xor ebx, ebx
.draw_key_entry:
    mov eax, ebx
    movss xmm0, dword ptr [rsp + 0x40]
    addss xmm0, dword ptr [rip + key_swatch_inset]
    cmp eax, 8
    jb .key_row_position
    sub eax, 8
    addss xmm0, dword ptr [rip + key_column_width]
.key_row_position:
    imul eax, eax, 20
    add eax, 12
    pxor xmm1, xmm1
    cvtsi2ss xmm1, eax
    addss xmm1, dword ptr [rsp + 0x44]

    lea rax, [rip + building_key_order]
    movzx eax, byte ptr [rax + rbx]
    cmp dword ptr [rsp + 0x38], 0
    jne .draw_key_label
    lea rcx, [rip + building_type_palette]
    mov eax, dword ptr [rcx + 4*rax]
    mov dword ptr [rsp + 0x20], eax
    mov rax, qword ptr [rsp + 0x30]
    mov qword ptr [rsp + 0x28], rax
    movaps xmm2, xmm0
    movaps xmm3, xmm1
    addss xmm2, dword ptr [rip + key_swatch_size]
    addss xmm3, dword ptr [rip + key_swatch_size]
    call draw_ui_rectangle
    jmp .next_key_entry

.draw_key_label:
    imul eax, eax, 24
    lea rcx, [rip + building_type_names]
    add rcx, rax
.print_key_label:
    movzx eax, byte ptr [rcx]
    inc rcx
    mov qword ptr [rsp + 0x48], rcx
    mov qword ptr [rsp + 0x50], rax
    addss xmm0, dword ptr [rip + key_label_offset]
    subss xmm1, dword ptr [rip + key_border]
    mulss xmm0, dword ptr [rip + ui_pixels_to_tiles]
    mulss xmm1, dword ptr [rip + ui_pixels_to_tiles]
    movss dword ptr [rip + ui_text_position], xmm0
    movss dword ptr [rip + ui_text_position + 4], xmm1
    call set_text_position
    lea rcx, [rip + string_format]
    lea rdx, [rsp + 0x68]
    mov r8, qword ptr [rsp + 0x30]
    call draw_ui_text
.next_key_entry:
    inc ebx
    cmp ebx, BUILDING_KEY_COUNT
    jb .draw_key_entry

    cmp dword ptr [rsp + 0x38], 0
    je .key_done
    cmp dword ptr [rsp + 0x84], 0
    je .restore_key_text
    cmp ebx, BUILDING_KEY_COUNT + SPAWN_INFO_ROWS
    jb .draw_spawn_info
.restore_key_text:
    mov rax, qword ptr [rsp + 0x78]
    mov qword ptr [rip + ui_text_position], rax
    movss xmm0, dword ptr [rsp + 0x78]
    movss xmm1, dword ptr [rsp + 0x7c]
    call set_text_position
    mov ecx, dword ptr [rsp + 0x80]
    mov dword ptr [rip + ui_text_color], ecx
    mov edx, ecx
    shr edx, 8
    mov r8d, ecx
    shr r8d, 16
    mov r9d, ecx
    shr r9d, 24
    call set_text_rgba
    jmp .key_done

.draw_spawn_info:
    mov eax, ebx
    sub eax, BUILDING_KEY_COUNT
    movss xmm0, dword ptr [rsp + 0x40]
    lea rcx, [rip + spawn_info_title]
    lea rdx, [rip + spawn_estimate_title]
    cmp dword ptr [rsp + 0x84], 2
    cmove rcx, rdx
    test eax, eax
    je .position_spawn_info
    lea ecx, [rax - 1]
    imul ecx, ecx, 48
    lea rdx, [rip + militia_spawn_lines]
    add rcx, rdx
.position_spawn_info:
    imul eax, eax, 20
    add eax, 174
    pxor xmm1, xmm1
    cvtsi2ss xmm1, eax
    addss xmm1, dword ptr [rsp + 0x44]
    jmp .print_key_label
.key_done:
    add rsp, 0x90
    pop rbx
    ret
draw_building_type_key_end:

building_types_label:
    .ascii "Building Types"
spawn_info_title:
    .byte 2f - 1f
1:  .ascii "Qualified at battle start"
2:
spawn_estimate_title:
    .byte 2f - 1f
1:  .ascii "Estimated eligible civilians"
2:

.macro use_name name
    .byte 2f - 1f
1:  .ascii "\name"
2:  .zero 23 - (2b - 1b)
.endm

building_key_order:
    .byte 1, 14, 2, 15, 3, 4, 5, 6
    .byte 7, 8, 9, 10, 11, 12, 13, 0

building_type_names:
    use_name Unknown
    use_name Residential
    use_name Commercial
    use_name Industrial
    use_name Education
    use_name Medical
    use_name Religious
    use_name Civic
    use_name Transportation
    use_name Entertainment
    use_name Agricultural
    use_name Military
    use_name Service
    use_name Outbuilding
    use_name "Inferred home"
    use_name "Inferred comm."

# Indexed by the inferred-use byte, including the GIS source bit.
statpanel_use_names:
    use_name ""
    use_name "INFERRED HOME"
    use_name "INFERRED COMM."
    use_name ""
    use_name ""
    use_name "GIS RESIDENTIAL"
    use_name "GIS COMMERCIAL"

.p2align 2
key_width:         .float 544.0
key_height:        .float 174.0
key_inner_width:   .float 542.0
key_inner_height:  .float 172.0
key_top_offset:    .float 262.0  # Panel height plus clearance above the toolbar.
key_margin:        .float 8.0
key_border:        .float 1.0
key_column_width:  .float 278.0
key_swatch_inset:  .float 12.0
key_swatch_size:   .float 12.0
key_label_offset:  .float 22.0
spawn_info_height: .float 102.0
battle_button_gap: .float 6.0
battle_button_width: .float 248.0
battle_button_height: .float 28.0

# Packed RGBA in the map's building-use order.
.p2align 2
building_type_palette:
    .byte 126, 133, 143, 255  # Unknown
    .byte  98, 201, 146, 255  # Residential
    .byte  82, 163, 249, 255  # Commercial
    .byte 238, 161,  81, 255  # Industrial
    .byte 176, 139, 245, 255  # Education
    .byte 243, 104, 116, 255  # Medical
    .byte 239, 203,  94, 255  # Religious
    .byte  92, 208, 220, 255  # Civic
    .byte  82, 105, 207, 255  # Transportation
    .byte 247, 146, 209, 255  # Entertainment
    .byte 184, 209,  91, 255  # Agricultural
    .byte  89, 139, 100, 255  # Military
    .byte 192, 140, 114, 255  # Service
    .byte 194, 197, 205, 255  # Outbuilding
    .byte  44, 132,  86, 255  # Inferred home
    .byte  47, 114, 172, 255  # Inferred commercial
