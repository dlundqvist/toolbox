VERSION		EQU	0
REVISION	EQU	2

DATE	MACRO
		dc.b '21.9.2025'
		ENDM

VERS	MACRO
		dc.b 'toolbox 0.2'
		ENDM

VSTRING	MACRO
		dc.b 'toolbox 0.2 (21.9.2025)',13,10,0
		ENDM

VERSTAG	MACRO
		dc.b 0,'$VER: toolbox 0.2 (21.9.2025)',0
		ENDM
