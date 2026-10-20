#!/bin/sh
# Re-run the Stage 4 tests against the integrated faithful code (app_vmmux).
# Each test links the same chunk files as in Stage 4; 4G/4H/4I also link src/vm_tags.c,
# because their tags are now defined there (with the data dictionary's power-up values).
# Every vm_config.h switch is set to the original behaviour (old email and logging
# called, no owner fixes), which is what these tests check.
# Usage (from the repository root): sh app_vmmux/stage4_tests/run.sh [outdir]
set -e
A=app_vmmux; O=${1:-build/stage4}; mkdir -p "$O"
CF="-DVM_OLD_EMAIL=1 -DVM_OLD_LOGGING=1 -DVM_FIX_DI_REAL_INPUTS=0 -DVM_FIX_AI_SCALING=0 -DVM_FIX_MUX_RESTART=0 -DVM_MUX_CONTINUOUS=0 -DVM_FIX_MUX_VALVE_ASSIGN=0 -DVM_FIX_V7=0 -std=c11 -O1 -g -Wall -Wextra -I$A/include -I$A/faithful -Iplatform/include -fsanitize=address,undefined -fno-omit-frame-pointer"
run() { k=$1; shift; gcc $CF "$@" -o "$O/$k" -lm && "./$O/$k" > "$O/$k.txt" 2>&1; r=$?; printf '%-4s %s\n' "$k" "$(tail -1 "$O/$k.txt")"; return $r; }
S="$A/stage4_tests/t4_support.c $A/src/ul_blocks.c"
F=$A/faithful; T=$A/stage4_tests
rc=0
run 4A  $T/test_4A.c  $T/4A_new.c $F/4A_faithful.c $S || rc=1
run 4B  $T/test_4B.c  $F/4B_faithful.c $S || rc=1
run 4C  $T/test_4C.c  $F/4C_faithful.c $S -DT4_STUB_TWOORFB22 || rc=1
run 4D  $T/test_4D.c  $F/4D_faithful.c $S || rc=1
run 4E  $T/test_4E.c  $F/4E_faithful.c $S || rc=1
run 4F1 $T/test_4F1.c $F/4F1_faithful.c $S || rc=1
run 4F2 $T/test_4F2.c $F/4F2_faithful.c $S || rc=1
run 4F3 $T/test_4F3.c $F/4F3_faithful.c $F/4D_faithful.c $F/4A_faithful.c $S -DT4_SYS_TAGS || rc=1
run 4F4 $T/test_4F4.c $F/4F4_faithful.c $F/4B_faithful.c $S || rc=1
run 4G  $T/test_4G.c  $F/4G_faithful.c $F/4A_faithful.c $A/src/vm_tags.c $A/src/ul_models.c $S -DT4_SYS_TAGS || rc=1
run 4H  $T/test_4H.c  $F/4H_faithful.c $F/4A_faithful.c $A/src/vm_tags.c $A/src/ul_models.c $S -DT4_SYS_TAGS || rc=1
run 4I  $T/test_4I.c  $F/4I_faithful.c $F/4A_faithful.c $A/src/vm_tags.c $A/src/ul_models.c $S -DT4_SYS_TAGS || rc=1
exit $rc
