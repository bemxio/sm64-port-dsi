.macro gsymbol sym addr
.global \sym
.equ \sym, \addr
#ifndef VERSION_JP
nop
nop
#endif
.endm

.text
gsymbol osTvType 0x80000300
gsymbol osRomType 0x80000304
gsymbol osRomBase 0x80000308
gsymbol osResetType 0x8000030C
gsymbol osCiCId 0x80000310
gsymbol osVersion 0x80000314
gsymbol osMemSize 0x80000318
gsymbol osAppNmiBuffer 0x8000031C
#if defined(VERSION_SH) || defined(VERSION_CN)
nop
nop
nop
nop
nop
nop
nop
nop
#ifdef VERSION_CN
nop
nop
nop
nop
#endif
#endif
