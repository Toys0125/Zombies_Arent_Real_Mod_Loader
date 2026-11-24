.intel_syntax noprefix
.text

.equ INFERRED_USE_MASK, 3

.globl battle_start, battle_start_end
.globl sample_civilian, sample_civilian_end, militia_source, militia_source_end
.globl count_living_militia, count_living_militia_end
.globl add_militia_after_tick, add_militia_after_tick_end
.globl rank_completed_battle, rank_completed_battle_end
.globl invalidate_inferred_building_uses, invalidate_inferred_building_uses_end

# Called in the battle initializer just after it sets the live battle phase.
# r14 is the game's Odin context at this call site.
battle_start:
    sub rsp, 40
    mov byte ptr [rip + count_militia_this_battle], 0
    mov byte ptr [rip + militia_spawn_info_ready], 0
    xor eax, eax
    mov qword ptr [rip + militia_qualified], rax
    mov qword ptr [rip + militia_qualified + 8], rax
    mov qword ptr [rip + militia_qualified + 16], rax
    xor ecx, ecx
    call select_militia_rules
    test rax, rax
    je .finish
    mov qword ptr [rip + active_militia_rules], rax
    call refresh_inferred_building_uses
    # Co-op installs the same outbreak seed before starting on both peers.
    # Solo play gets a fresh roll. Pass the seed through the unused circle
    # center and mark the source argument to select the sampling path.
    mov ecx, dword ptr [rip + outbreak_seed]
    cmp byte ptr [rip + coop_starting_battle], 0
    jne .pass_seed
    rdtsc
    xor eax, edx
    mov ecx, eax
.pass_seed:
    mov eax, 0x3f800000
    movd xmm1, eax
    movd xmm2, eax
    mov r9d, 0x8d
    mov qword ptr [rsp + 32], r14
    call arm_civilians
    test rax, rax
    jle .no_militia_spawned
    add qword ptr [rip + militia_battle_count], rax
    mov rcx, rax
    call format_spawn_info
    call recount_units
    jmp .finish
.no_militia_spawned:
    xor ecx, ecx
    call format_spawn_info

.finish:
    # Restore the argument and tail-call the original battle-start sound routine.
    mov cl, 5
    add rsp, 40
    jmp play_battle_start_sound
battle_start_end:

# Detour from the arming loop after its stock civilian-kind check. The marked
# call selects candidates by rolling once per civilian. The stock routine
# then converts all selected candidates using its usual militia behavior.
sample_civilian:
    cmp byte ptr [rsp + 0x36], 0x8d
    je .classify
    mov rax, qword ptr [rip + civilian_positions]
    jmp check_circle_distance
.classify:
    # Look up the footprint containing the starting position, or the
    # closest footprint within the game's 3-meter search distance.
    mov rax, qword ptr [rip + civilian_positions]
    mov rcx, qword ptr [rax + 8*r15]
    call classify_starting_civilian
    mov r9, qword ptr [rsp + 0x50]
    mov edx, eax
    lea rax, [rip + militia_qualified]
    inc qword ptr [rax + 8*rdx]
    # Mix the civilian's index with the battle seed. The 32-bit result is
    # compared as unsigned against a threshold in [0, 2**32].
    movd eax, xmm8
    add eax, r15d
    mov r10d, eax
    shr r10d, 16
    xor eax, r10d
    imul eax, eax, 0x7feb352d
    mov r10d, eax
    shr r10d, 15
    xor eax, r10d
    imul eax, eax, 0x846ca68b
    mov r10d, eax
    shr r10d, 16
    xor eax, r10d
    mov rcx, qword ptr [rip + active_militia_rules]
    cmp rax, qword ptr [rcx + 8*rdx]
    jb keep_candidate
    jmp next_civilian
sample_civilian_end:

# Store the game's real militia source ID for attribution and statistics.
militia_source:
    movzx eax, byte ptr [rsp + 0x36]
    cmp al, 0x8d
    jne .store_source
    mov al, 0x0d
.store_source:
    jmp store_unit_source
militia_source_end:

# The game tracks militia and Call to Arms recruits as officers. Include
# both kinds of armed civilian in the living-civilian tally.
count_living_militia:
    inc qword ptr [rsp + 0x30]
    cmp byte ptr [rip + count_militia_this_battle], 0
    je .count_done
    cmp qword ptr [rip + battle_phase], 0
    je .count_done
    cmp qword ptr [rip + officer_source_count], 0
    je .count_done

    # Reuse spill slots in the stock recount frame while looking up the
    # current unit's stable ID in the game's officer-source hash table.
    mov qword ptr [rsp + 0x68], r8
    mov qword ptr [rsp + 0x50], r9
    mov qword ptr [rsp + 0x60], r10
    mov qword ptr [rsp + 0x58], r11
    mov rax, qword ptr [rip + unit_ids]
    mov rcx, qword ptr [rax + 8*r14]
    call is_armed_civilian
    add qword ptr [rsp + 0x48], rax
    mov r8, qword ptr [rsp + 0x68]
    mov r9, qword ptr [rsp + 0x50]
    mov r10, qword ptr [rsp + 0x60]
    mov r11, qword ptr [rsp + 0x58]
.count_done:
    jmp next_counted_unit
count_living_militia_end:

# The simulation's parallel tally has its own aggregation path. Add living
# militia after it has applied all deaths for the tick. The stock epilogue
# restores the nonvolatile registers and loads its return value from the stack.
add_militia_after_tick:
    cmp byte ptr [rip + count_militia_this_battle], 0
    je .tick_done
    cmp qword ptr [rip + battle_phase], 0
    je .tick_done
    cmp qword ptr [rip + officer_source_count], 0
    je .tick_done
    mov r12, qword ptr [rip + unit_count]
    test r12, r12
    jle .tick_done
    mov rsi, qword ptr [rip + unit_kinds]
    mov rdi, qword ptr [rip + unit_ids]
    xor r14d, r14d
    xor r15d, r15d
.check_fighter:
    cmp byte ptr [rsi + r14], 2
    jne .next_fighter
    mov rcx, qword ptr [rdi + 8*r14]
    call is_armed_civilian
    add r15, rax
.next_fighter:
    inc r14
    cmp r14, r12
    jl .check_fighter
    add qword ptr [rip + live_civilian_count], r15
.tick_done:
    jmp finish_ticking_units
add_militia_after_tick_end:

# Use the game's existing unranked results path for battles using the mod.
# Local stage results have already been recorded by the time this runs.
rank_completed_battle:
    cmp byte ptr [rip + count_militia_this_battle], 0
    jne show_modded_result
    cmp byte ptr [rip + steam_available], 1
    jne finish_ranking
    jmp check_stock_ranking
rank_completed_battle_end:

invalidate_inferred_building_uses:
    cmp byte ptr [rip + militia_spawn_info_ready], 2
    jne .invalidate_buildings
    mov byte ptr [rip + militia_spawn_info_ready], 0
.invalidate_buildings:
    mov qword ptr [rip + inferred_building_count], 0
    mov byte ptr [rip + building_grid_rebuild_flag], 1
    jmp resume_building_grid_rebuild
invalidate_inferred_building_uses_end:

# Leaf using only volatile registers and no stack scratch space. Both callers
# run after the simulation workers have joined and the source table is stable.
is_armed_civilian:
    mov r11, rcx
    mov rax, qword ptr [rip + officer_source_table]
    and rax, -64
    movabs rdx, 0x94d049bb133111eb
    mul rdx
    mov r8, rax
    mov r9, rdx
    mov rax, r11
    xor rax, 8
    movabs rdx, 0x9e3779b97f4a7c15
    mul rdx
    xor r9, rdx
    xor r9, r8
    xor rax, r9
    movabs rdx, 0xbf58476d1ce4e5b9
    mul rdx
    xor rdx, rax
    btr rdx, 63
    mov r10d, 1
    test rdx, rdx
    cmovne r10, rdx

    # The native table stores keys, one-byte sources, then slot hashes.
    mov rcx, qword ptr [rip + officer_source_table]
    mov r8, rcx
    and r8, -64
    mov edx, 1
    shl rdx, cl
    lea rcx, [r8 + 8*rdx]
    lea r9, [rcx + rdx]
    dec rdx
    mov rax, r10
.probe_source:
    and rax, rdx
    mov rcx, qword ptr [r9 + 8*rax]
    test rcx, rcx
    je .source_missing
    cmp rcx, r10
    jne .next_source
    cmp qword ptr [r8 + 8*rax], r11
    je .check_source
.next_source:
    inc rax
    jmp .probe_source
.check_source:
    mov rcx, r9
    sub rcx, rdx
    movzx ecx, byte ptr [rcx + rax - 1]
    mov eax, 1
    cmp cl, 0x0d
    je .source_found
    cmp cl, 0x1f
    sete al
.source_found:
    ret
.source_missing:
    xor eax, eax
    ret

.include "building_types.s"
.include "mod_settings.s"
