#!/bin/bash
# Talk to the Windows test VM.
#   vm.sh ssh <command...>       run a cmd.exe command over SSH
#   vm.sh put <local> [remote]   copy a file into C:\dx11on10 (or a given remote path)
#   vm.sh get <remote> <local>   copy a file out of the VM
#   vm.sh cmd <file.cmd>         upload a .cmd and run it over SSH
#   vm.sh gui <file.cmd>         upload a .cmd and run it in the logged-in desktop session
#   vm.sh system <file.cmd>      upload a .cmd and run it as SYSTEM
set -e
VM=${VM:-kupat@192.168.87.129}
KEY=${KEY:-$HOME/.ssh/thermohammer_vm}
SSH=(ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=10 "$VM")
SCP=(scp -i "$KEY" -o BatchMode=yes -q)
REMOTE=C:/dx11on10

run_task() {
  local name=$1 cmd=$2 extra=$3
  "${SSH[@]}" "schtasks /create /tn $name /tr C:\\dx11on10\\$cmd /sc once /st 23:59 $extra /f >nul && schtasks /run /tn $name >nul && echo started $name"
}

case "$1" in
  ssh) shift; "${SSH[@]}" "$*" ;;
  put) "${SCP[@]}" "$2" "$VM:${3:-$REMOTE/}" ;;
  get) "${SCP[@]}" "$VM:$2" "$3" ;;
  cmd) "${SCP[@]}" "$2" "$VM:$REMOTE/"; "${SSH[@]}" "C:\\dx11on10\\$(basename "$2")" ;;
  gui) "${SCP[@]}" "$2" "$VM:$REMOTE/"; run_task dx11on10_gui "$(basename "$2")" "/it" ;;
  system) "${SCP[@]}" "$2" "$VM:$REMOTE/"; run_task dx11on10_sys "$(basename "$2")" "/ru SYSTEM" ;;
  *) echo "usage: vm.sh ssh|put|get|cmd|gui|system ..." >&2; exit 1 ;;
esac
