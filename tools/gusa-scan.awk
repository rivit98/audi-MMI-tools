# gusa-scan.awk - list the functions of an SH4 binary that contain GCC gUSA
# atomic sequences (-matomic-model=soft-gusa), which QNX doesn't restart
# when a thread is switched out inside one (docs/cpp-apps.md). One line per
# sequence, the function's name. `make gusa-scan` runs it over the apps:
#   sh4-linux-gnu-objdump -d --no-show-raw-insn build/<app> | awk -f tools/gusa-scan.awk
# A sequence is mova <end>,r0 ... mov r15,rN ... mov #-<len>,r15, within a
# few instructions (GCC may schedule a nop in between). Matching the
# mov #-N,r15 alone also hits literal pools decoded as code.
/^[0-9a-f]+ <.*>:/ { fn = $2; since = 99; next }
{ ins = $0; sub(/^[ \t]*[0-9a-f]+:[ \t]*/, "", ins); since++ }
ins ~ /^mova\t/ { since = 0 }
ins ~ /^mov\t#-[0-9]+,r15$/ && since <= 4 { print fn }
