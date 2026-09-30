set architecture i386
set pagination off
set confirm off
set remote verbose-resume-packet off
set logging file artifacts/diagnostics/xemu-xmv-coeff-20260823/gdb.log
set logging overwrite on
set logging on
target remote 127.0.0.1:12345
set $poll = 0

hbreak *0x002D5579
commands
  silent
  set $eax = 1
  set $eip = *(unsigned int *)$esp
  set $esp = $esp + 8
  continue
end

hbreak *0x002D5272
commands
  silent
  set $eax = 1
  set $eip = *(unsigned int *)$esp
  set $esp = $esp + 20
  continue
end

hbreak *0x002D54AC
commands
  silent
  set $poll = $poll + 1
  set $state = *(unsigned int *)($esp + 8)
  set {unsigned int}($state + 0) = $poll
  set {unsigned int}($state + 4) = 0
  set {unsigned int}($state + 8) = 0
  set {unsigned int}($state + 12) = 0
  set {unsigned int}($state + 16) = 0
  set {unsigned short}($state + 20) = 0
  if ($poll >= 5 && ($poll % 20) < 3)
    set {unsigned short}($state + 4) = 0x0010
  end
  if ($poll >= 30 && ($poll % 30) < 3)
    set {unsigned char}($state + 6) = 0xFF
  end
  set $eax = 0
  set $eip = *(unsigned int *)$esp
  set $esp = $esp + 12
  continue
end

hbreak *0x002580E8
continue
printf "XMV coefficient checkpoint reached: poll=%u eax_mask=%08X ebp=%08X\n", $poll, $eax, $ebp
info registers eax ebp esp eip
x/64hd $ebp-128
dump binary memory artifacts/diagnostics/xemu-xmv-coeff-20260823/original-coeff.bin $ebp-128 $ebp
detach
quit