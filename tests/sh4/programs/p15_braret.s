! A function that comes back to its caller with a bra to the instruction after the caller's jsr
! instead of an rts, as Katana's exception stub does in 18 Wheeler and F355 Challenge (its
! dispatcher at 0x8c0041c6 ends with `bra 0x8c004174`). The caller's code after the jsr is that
! address, so the branch is a return; as a tail call it ran that code twice.
! _brcont stands for the caller's continuation, which discovery split into a function of its own.
! Linked at -Ttext=0x8c050000 -e 0x8c050000 (hand-assembled: words in comments).
	.text
	.global _brcont
	.global _brback
_brcont:
	add	#1,r5		! 7501: counts the passes through the continuation
	lds.l	@r15+,pr	! 4f26
	rts			! 000b
	mov	r5,r0		! 6053
_brback:
	bra	_brcont		! affa: back to the caller's continuation, not an rts
	add	#10,r4		! 740a
