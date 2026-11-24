; Static, initialized TLS bytes for the two CRT-free EXE fixtures.
; They must be present before the Windows loader invokes the first TLS callback.
OPTION DOTNAME
OPTION CASEMAP:NONE

.tls$X SEGMENT ALIGN(4) 'DATA'
IFDEF TLS_SLOT0_ARRAY
    PUBLIC t_array
t_array BYTE 1400h DUP (0FFh)
ELSE
    PUBLIC g_span
g_span LABEL DWORD
    word_index = 0
    REPT 2048
        DD (0A5000000h XOR (word_index * 09E3779B1h)) AND 0FFFFFFFFh
        word_index = word_index + 1
    ENDM
ENDIF
.tls$X ENDS
END
