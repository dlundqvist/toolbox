VERSION		EQU	0
REVISION	EQU	1

DATE	MACRO
		dc.b '15.9.2025'
		ENDM

VERS	MACRO
		dc.b 'toolbox 0.1'
		ENDM

VSTRING	MACRO
		dc.b 'toolbox 0.1 (15.9.2025)',13,10,0
		ENDM

VERSTAG	MACRO
		dc.b 0,'$VER: toolbox 0.1 (15.9.2025)',0
		ENDM
