.set noreorder // don't insert nops after branches

#include "macros.inc"


.section .text, "ax"

glabel __osSetSR
    mtc0  $a0, $12
    nop
    jr    $ra
     nop

